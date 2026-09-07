import Foundation

enum TrainingSource: String, Codable, CaseIterable, Identifiable, Hashable {
    case glove
    case demo

    var id: String { rawValue }

    var title: String {
        switch self {
        case .glove: return "Luva"
        case .demo: return "Demonstração"
        }
    }

    var isReal: Bool { self == .glove }
}

enum ActiveTrainingState: String, Codable, Equatable {
    case running
    case paused
}

struct PunchRecord: Identifiable, Codable, Hashable {
    let id: UUID
    let receivedAt: Date
    let devicePunchID: UInt16?
    let deviceTimestampMicros: UInt32?
    let elapsedDeviceMicros: UInt64?
    /// An uncalibrated 12-bit FSR sensor count. It is never a physical-force value.
    let fsrADC: UInt16?
    let source: TrainingSource

    init(
        id: UUID = UUID(),
        receivedAt: Date = Date(),
        devicePunchID: UInt16? = nil,
        deviceTimestampMicros: UInt32? = nil,
        elapsedDeviceMicros: UInt64? = nil,
        fsrADC: UInt16? = nil,
        source: TrainingSource
    ) {
        self.id = id
        self.receivedAt = receivedAt
        self.devicePunchID = devicePunchID
        self.deviceTimestampMicros = deviceTimestampMicros
        self.elapsedDeviceMicros = elapsedDeviceMicros
        self.fsrADC = fsrADC
        self.source = source
    }
}

/// Codable state contains only a checkpointed active duration. Live elapsed time is
/// deliberately maintained from the process monotonic clock by `TrainingViewModel`.
/// That prevents a wall-clock change, pause, or relaunch from being counted as training.
struct ActiveTrainingSession: Identifiable, Codable {
    let id: UUID
    let source: TrainingSource
    let startedAt: Date
    var accumulatedDuration: TimeInterval
    /// A wall-clock marker for recovery/audit only. It is not used to calculate duration.
    var resumedAt: Date?
    var state: ActiveTrainingState
    var punches: [PunchRecord]

    init(source: TrainingSource, now: Date = Date()) {
        id = UUID()
        self.source = source
        startedAt = now
        accumulatedDuration = 0
        resumedAt = now
        state = .running
        punches = []
    }

    mutating func checkpoint(adding elapsed: TimeInterval, at now: Date = Date()) {
        guard state == .running else { return }
        accumulatedDuration += max(0, elapsed)
        resumedAt = now
    }

    mutating func pause(adding elapsed: TimeInterval, at now: Date = Date()) {
        guard state == .running else { return }
        accumulatedDuration += max(0, elapsed)
        resumedAt = nil
        state = .paused
    }

    /// A running session found after relaunch cannot prove how long the app was away.
    /// Preserve its last checkpoint and place it in pause without adding wall-clock time.
    mutating func pauseForRecovery(at _: Date) {
        guard state == .running else { return }
        resumedAt = nil
        state = .paused
    }

    mutating func resume(at now: Date = Date()) {
        guard state == .paused else { return }
        resumedAt = now
        state = .running
    }
}

struct TrainingSession: Identifiable, Codable, Hashable {
    let id: UUID
    let source: TrainingSource
    let startedAt: Date
    let endedAt: Date
    let duration: TimeInterval
    let punches: [PunchRecord]

    init(from active: ActiveTrainingSession, endedAt: Date = Date()) {
        id = active.id
        source = active.source
        startedAt = active.startedAt
        self.endedAt = endedAt
        duration = active.accumulatedDuration
        punches = active.punches
    }

    var punchCount: Int { punches.count }
}

struct UserSettings: Codable, Hashable {
    var hapticsEnabled: Bool = true
    var dailyPunchGoal: Int = 50

    static let `default` = UserSettings()
}

struct AppSnapshot: Codable {
    static let currentSchemaVersion = 1

    var schemaVersion: Int
    var realSessions: [TrainingSession]
    var demoSessions: [TrainingSession]
    var activeSession: ActiveTrainingSession?
    var settings: UserSettings
    var lastUpdatedAt: Date

    static func empty(now: Date = Date()) -> AppSnapshot {
        AppSnapshot(
            schemaVersion: currentSchemaVersion,
            realSessions: [],
            demoSessions: [],
            activeSession: nil,
            settings: .default,
            lastUpdatedAt: now
        )
    }
}

struct Achievement: Identifiable, Hashable {
    let id: String
    let title: String
    let detail: String
    let symbol: String
    let completed: Bool
    let currentValue: Int
    let targetValue: Int
}

struct ProgressSummary {
    let totalRealPunches: Int
    let completedRealSessions: Int
    let punchesToday: Int
    let dailyGoal: Int
    let achievements: [Achievement]

    var dailyProgress: Double {
        min(1, Double(punchesToday) / Double(max(1, dailyGoal)))
    }

    static func make(from snapshot: AppSnapshot, calendar: Calendar = .current) -> ProgressSummary {
        let qualifyingSessions = snapshot.realSessions.filter { !$0.punches.isEmpty }
        let totalPunches = qualifyingSessions.reduce(0) { $0 + $1.punchCount }
        let todayPunches = qualifyingSessions
            .flatMap(\.punches)
            .filter { calendar.isDateInToday($0.receivedAt) }
            .count

        let milestones = [
            Achievement(
                id: "first-10",
                title: "Primeiros 10",
                detail: "Regista 10 golpes reais.",
                symbol: "bolt.fill",
                completed: totalPunches >= 10,
                currentValue: totalPunches,
                targetValue: 10
            ),
            Achievement(
                id: "fifty",
                title: "Ritmo de 50",
                detail: "Acumula 50 golpes reais.",
                symbol: "flame.fill",
                completed: totalPunches >= 50,
                currentValue: totalPunches,
                targetValue: 50
            ),
            Achievement(
                id: "three-sessions",
                title: "Constância",
                detail: "Conclui 3 sessões reais com golpes registados.",
                symbol: "calendar.badge.checkmark",
                completed: qualifyingSessions.count >= 3,
                currentValue: qualifyingSessions.count,
                targetValue: 3
            ),
            Achievement(
                id: "hundred",
                title: "Centena",
                detail: "Acumula 100 golpes reais.",
                symbol: "trophy.fill",
                completed: totalPunches >= 100,
                currentValue: totalPunches,
                targetValue: 100
            )
        ]

        return ProgressSummary(
            totalRealPunches: totalPunches,
            completedRealSessions: qualifyingSessions.count,
            punchesToday: todayPunches,
            dailyGoal: max(1, snapshot.settings.dailyPunchGoal),
            achievements: milestones
        )
    }
}

extension TimeInterval {
    var clockText: String {
        let rounded = max(0, Int(self.rounded(.down)))
        return String(format: "%02d:%02d", rounded / 60, rounded % 60)
    }
}
