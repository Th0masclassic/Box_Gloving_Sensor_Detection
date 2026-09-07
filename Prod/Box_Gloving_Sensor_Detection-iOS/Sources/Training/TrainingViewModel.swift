import Combine
import Foundation
import UIKit

@MainActor
final class TrainingViewModel: ObservableObject {
    @Published private(set) var snapshot: AppSnapshot
    @Published private(set) var activeSession: ActiveTrainingSession?
    @Published private(set) var lastPunch: PunchRecord?
    @Published private(set) var currentCadence: Int?
    @Published private(set) var storageNotice: String?
    @Published private(set) var safetyNotice: String?
    @Published private(set) var elapsedDisplay = 0
    @Published private(set) var storeLoadState: StoreLoadState
    @Published var selectedSource: TrainingSource

    let bluetooth: GloveBluetoothManager

    private let store: LocalStore
    private let writer: SnapshotWriter
    private let demoSource = DemoPunchSource()
    private var elapsedTimer: Timer?
    private var runningSegmentStartedAtUptime: TimeInterval?
    private var lastAcceptedPunchUptime: TimeInterval?

    init(store: LocalStore = LocalStore(), bluetooth: GloveBluetoothManager = GloveBluetoothManager()) {
        self.store = store
        self.bluetooth = bluetooth
        let loaded = store.load()
        snapshot = loaded.snapshot
        activeSession = loaded.snapshot.activeSession
        storeLoadState = loaded.state
        selectedSource = loaded.snapshot.activeSession?.source ?? .glove
        writer = SnapshotWriter(store: store)

        writer.onFailure = { [weak self] _ in
            self?.storageNotice = "Não foi possível guardar a alteração localmente. Os dados em memória continuam disponíveis."
        }

        if let message = loaded.state.userMessage {
            storageNotice = message
        }

        if var recoveredSession = activeSession, recoveredSession.state == .running {
            recoveredSession.pauseForRecovery(at: loaded.snapshot.lastUpdatedAt)
            activeSession = recoveredSession
            snapshot.activeSession = recoveredSession
            snapshot.lastUpdatedAt = Date()
            safetyNotice = "O treino anterior foi colocado em pausa ao reabrir a app."
            persistSnapshot(urgency: .immediate)
        }

        bluetooth.onPunch = { [weak self] signal in
            self?.receiveGlovePunch(signal)
        }
        bluetooth.onConnectionLost = { [weak self] reason in
            self?.pauseForSafety(reason: reason)
        }

        refreshElapsedDisplay()
    }

    deinit {
        elapsedTimer?.invalidate()
        demoSource.stop()
    }

    var settings: UserSettings { snapshot.settings }
    var progression: ProgressSummary { ProgressSummary.make(from: snapshot) }
    var realSessions: [TrainingSession] { snapshot.realSessions }
    var demoSessions: [TrainingSession] { snapshot.demoSessions }
    var activePunchCount: Int { activeSession?.punches.count ?? 0 }
    var isTrainingRunning: Bool { activeSession?.state == .running }
    var canWriteLocalData: Bool { storeLoadState.permitsWrites }
    var needsRecoveryReset: Bool {
        if case .recoveryRequired = storeLoadState { return true }
        return false
    }

    func connectGlove() {
        safetyNotice = nil
        bluetooth.connect()
    }

    func connect(to device: GloveDiscoveredDevice) {
        safetyNotice = nil
        bluetooth.connect(to: device.id)
    }

    func disconnectGlove() {
        pauseForSafety(reason: "A ligação foi terminada; o treino foi colocado em pausa.")
        bluetooth.disconnect()
    }

    func startOrResumeTraining() {
        guard canWriteLocalData else {
            storageNotice = "Resolva a recuperação dos dados locais antes de iniciar um treino."
            return
        }
        safetyNotice = nil

        if var activeSession {
            guard activeSession.state == .paused else { return }
            if activeSession.source == .glove, !bluetooth.isReady {
                safetyNotice = "Ligue a luva antes de retomar o treino real."
                return
            }
            activeSession.resume()
            self.activeSession = activeSession
            selectedSource = activeSession.source
            startRunningSegment()
            clearPresentationFeedback()
            if activeSession.source == .demo { startDemoSource() }
            checkpointActiveSession(urgency: .immediate)
            startElapsedTimer()
            return
        }

        guard selectedSource == .demo || bluetooth.isReady else {
            safetyNotice = "Ligue a luva e aguarde o estado “Luva pronta”."
            return
        }

        let newSession = ActiveTrainingSession(source: selectedSource)
        activeSession = newSession
        clearPresentationFeedback()
        startRunningSegment()
        if selectedSource == .demo { startDemoSource() }
        checkpointActiveSession(urgency: .immediate)
        startElapsedTimer()
    }

    func pauseTraining() {
        guard var activeSession, activeSession.state == .running else { return }
        let elapsed = finishRunningSegment()
        activeSession.pause(adding: elapsed)
        self.activeSession = activeSession
        demoSource.stop()
        clearPresentationFeedback()
        stopElapsedTimer()
        checkpointActiveSession(urgency: .immediate, keepBackgroundTime: true)
    }

    func endTraining() {
        guard var activeSession else { return }
        if activeSession.state == .running {
            activeSession.pause(adding: finishRunningSegment())
        }
        demoSource.stop()
        stopElapsedTimer()
        let finished = TrainingSession(from: activeSession)
        if finished.source == .glove {
            snapshot.realSessions.insert(finished, at: 0)
        } else {
            snapshot.demoSessions.insert(finished, at: 0)
        }
        self.activeSession = nil
        snapshot.activeSession = nil
        snapshot.lastUpdatedAt = Date()
        clearPresentationFeedback()
        persistSnapshot(urgency: .immediate, keepBackgroundTime: true)
    }

    func pauseForSafety(reason: String) {
        guard activeSession?.state == .running else { return }
        pauseTraining()
        safetyNotice = reason
    }

    func handleScenePhase(isActive: Bool) {
        if isActive {
            bluetooth.setForegroundAllowed(true)
        } else {
            pauseForSafety(reason: "Treino em pausa porque a app saiu do primeiro plano.")
            bluetooth.setForegroundAllowed(false)
        }
    }

    func setHapticsEnabled(_ enabled: Bool) {
        guard canWriteLocalData else { return }
        snapshot.settings.hapticsEnabled = enabled
        snapshot.lastUpdatedAt = Date()
        persistSnapshot(urgency: .immediate)
    }

    func setDailyGoal(_ value: Int) {
        guard canWriteLocalData else { return }
        snapshot.settings.dailyPunchGoal = min(500, max(10, value))
        snapshot.lastUpdatedAt = Date()
        persistSnapshot(urgency: .immediate)
    }

    func exportDocument() -> ExportDocument? {
        guard canWriteLocalData else {
            storageNotice = "A exportação foi desativada enquanto os dados locais exigem recuperação."
            return nil
        }
        do {
            return ExportDocument(data: try store.exportData(for: snapshot))
        } catch {
            storageNotice = "Não foi possível preparar a exportação local."
            return nil
        }
    }

    /// Explicit user-triggered reset. It is also the only recovery action that may
    /// delete unreadable files; pending asynchronous writes are cancelled first.
    func deleteAllLocalData() {
        demoSource.stop()
        stopElapsedTimer()
        bluetooth.disconnect()
        writer.deleteAll { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                self.snapshot = .empty()
                self.activeSession = nil
                self.selectedSource = .glove
                self.clearPresentationFeedback()
                self.storeLoadState = .ready
                self.storageNotice = "Os dados locais foram apagados deste iPhone."
                self.safetyNotice = nil
            case .failure:
                self.storageNotice = "Não foi possível apagar os dados locais."
            }
        }
    }

    func dismissStorageNotice() {
        storageNotice = nil
    }

    func dismissSafetyNotice() {
        safetyNotice = nil
    }

    private func receiveGlovePunch(_ signal: GlovePunchSignal) {
        guard var activeSession,
              activeSession.source == .glove,
              activeSession.state == .running else { return }
        let record = PunchRecord(
            receivedAt: signal.receivedAt,
            devicePunchID: signal.punchID,
            deviceTimestampMicros: signal.triggerTimeMicros,
            elapsedDeviceMicros: signal.elapsedDeviceMicros,
            fsrADC: signal.fsrADC,
            source: .glove
        )
        activeSession.punches.append(record)
        self.activeSession = activeSession
        acceptPunchPresentation(record)
        checkpointActiveSession(urgency: .coalesced)
        playPunchHapticIfEnabled()
    }

    private func startDemoSource() {
        demoSource.start { [weak self] signal in
            self?.receiveDemoPunch(signal)
        }
    }

    private func receiveDemoPunch(_ signal: DemoPunchSignal) {
        guard var activeSession,
              activeSession.source == .demo,
              activeSession.state == .running else { return }
        let record = PunchRecord(id: signal.id, receivedAt: signal.receivedAt, source: .demo)
        activeSession.punches.append(record)
        self.activeSession = activeSession
        acceptPunchPresentation(record)
        checkpointActiveSession(urgency: .coalesced)
        playPunchHapticIfEnabled()
    }

    private func acceptPunchPresentation(_ record: PunchRecord) {
        let now = ProcessInfo.processInfo.systemUptime
        if let lastAcceptedPunchUptime {
            let interval = now - lastAcceptedPunchUptime
            if interval >= 0.25, interval <= 30 {
                currentCadence = Int((60 / interval).rounded())
            } else {
                currentCadence = nil
            }
        } else {
            currentCadence = nil
        }
        lastAcceptedPunchUptime = now
        lastPunch = record
    }

    private func clearPresentationFeedback() {
        lastPunch = nil
        currentCadence = nil
        lastAcceptedPunchUptime = nil
    }

    private func playPunchHapticIfEnabled() {
        guard snapshot.settings.hapticsEnabled else { return }
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    private func startRunningSegment() {
        runningSegmentStartedAtUptime = ProcessInfo.processInfo.systemUptime
    }

    private func finishRunningSegment() -> TimeInterval {
        defer { runningSegmentStartedAtUptime = nil }
        guard let started = runningSegmentStartedAtUptime else { return 0 }
        return max(0, ProcessInfo.processInfo.systemUptime - started)
    }

    private func currentLiveDuration() -> TimeInterval {
        guard let activeSession else { return 0 }
        guard activeSession.state == .running, let started = runningSegmentStartedAtUptime else {
            return activeSession.accumulatedDuration
        }
        return activeSession.accumulatedDuration + max(0, ProcessInfo.processInfo.systemUptime - started)
    }

    private func checkpointActiveSession(
        urgency: SnapshotWriter.Urgency,
        keepBackgroundTime: Bool = false
    ) {
        if var activeSession, activeSession.state == .running {
            let elapsed = finishRunningSegment()
            activeSession.checkpoint(adding: elapsed)
            self.activeSession = activeSession
            startRunningSegment()
        }
        snapshot.activeSession = activeSession
        snapshot.lastUpdatedAt = Date()
        persistSnapshot(urgency: urgency, keepBackgroundTime: keepBackgroundTime)
        refreshElapsedDisplay()
    }

    private func persistSnapshot(urgency: SnapshotWriter.Urgency, keepBackgroundTime: Bool = false) {
        guard canWriteLocalData else { return }
        let snapshotToWrite = snapshot
        let backgroundLease = BackgroundSaveLease()
        if keepBackgroundTime {
            backgroundLease.identifier = UIApplication.shared.beginBackgroundTask(withName: "Guardar treino") {
                backgroundLease.end()
            }
        }
        writer.persist(snapshotToWrite, urgency: urgency) { [weak self] result in
            backgroundLease.end()
            if case .failure = result {
                self?.storageNotice = "Não foi possível guardar a alteração localmente. Os dados em memória continuam disponíveis."
            }
        }
    }

    private func startElapsedTimer() {
        guard elapsedTimer == nil else { return }
        elapsedTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.refreshElapsedDisplay() }
        }
        refreshElapsedDisplay()
    }

    private func stopElapsedTimer() {
        elapsedTimer?.invalidate()
        elapsedTimer = nil
        refreshElapsedDisplay()
    }

    private func refreshElapsedDisplay() {
        elapsedDisplay = Int(currentLiveDuration().rounded(.down))
    }
}

private final class BackgroundSaveLease {
    var identifier: UIBackgroundTaskIdentifier = .invalid

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}
