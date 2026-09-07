import Combine
import CoreBluetooth
import Foundation

enum GloveConnectionState: Equatable {
    case idle
    case bluetoothUnavailable(String)
    case scanning
    case connecting(String)
    case subscribing(String)
    case handshaking(String)
    case ready(GloveDeviceInfo)
    case reconnecting
    case failed(String)

    var title: String {
        switch self {
        case .idle: return "Desligada"
        case let .bluetoothUnavailable(reason): return reason
        case .scanning: return "À procura de luvas"
        case let .connecting(name): return "A ligar a \(name)"
        case .subscribing: return "A preparar notificações"
        case .handshaking: return "A preparar a luva"
        case .ready: return "Luva pronta"
        case .reconnecting: return "A tentar voltar a ligar"
        case let .failed(reason): return reason
        }
    }

    var isReady: Bool {
        if case .ready = self { return true }
        return false
    }

    var isWorking: Bool {
        switch self {
        case .scanning, .connecting, .subscribing, .handshaking, .reconnecting:
            return true
        default:
            return false
        }
    }
}

struct GloveDeviceInfo: Equatable {
    let name: String
    let accelerometerRateHz: UInt16
    let magnetometerRateHz: UInt16
}

struct GloveDiscoveredDevice: Identifiable, Equatable {
    let id: UUID
    let name: String
    let rssi: Int
}

struct GlovePunchSignal: Equatable {
    let punchID: UInt16
    let triggerTimeMicros: UInt32
    let elapsedDeviceMicros: UInt64?
    let fsrADC: UInt16
    let receivedAt: Date
}

/// Main-queue CoreBluetooth adapter for the firmware's v2 single-characteristic
/// service. It requests only event and statistics capabilities (`47 42 02 05`).
final class GloveBluetoothManager: NSObject, ObservableObject {
    @Published private(set) var connectionState: GloveConnectionState = .idle
    @Published private(set) var discoveredDevices: [GloveDiscoveredDevice] = []
    @Published private(set) var missedTransportFrames: UInt32 = 0
    @Published private(set) var latestStats: [UInt8: [UInt32]] = [:]
    @Published private(set) var lastProtocolIssue: String?

    var onPunch: ((GlovePunchSignal) -> Void)?
    var onConnectionLost: ((String) -> Void)?

    private let serviceUUID = CBUUID(string: GloveProtocol.serviceUUID)
    private let characteristicUUID = CBUUID(string: GloveProtocol.dataCharacteristicUUID)
    private var central: CBCentralManager!
    private var activePeripheral: CBPeripheral?
    private var discoveredPeripherals: [UUID: CBPeripheral] = [:]
    private var dataCharacteristic: CBCharacteristic?
    private var peripheralName = "SMART_BOXING_GLOVE"
    private var userRequestedConnection = false
    private var foregroundAllowed = true
    private var handshake: HandshakeState = .idle
    private var activeSessionID: UInt16?
    private var lastSequence: UInt16?
    private var recentPunchIDs = Set<UInt16>()
    private var punchIDOrder: [UInt16] = []
    private var timestampUnwrapper = DeviceTimestampUnwrapper()
    private var statsChunkTotal: UInt8?
    private var reconnectAttempt = 0
    private var reconnectWorkItem: DispatchWorkItem?
    private var phaseTimeout: DispatchWorkItem?
    private var phaseToken = UUID()

    private enum HandshakeState: Equatable {
        case idle
        case awaitingAcknowledgement
        case ready
    }

    override init() {
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
    }

    var isReady: Bool { connectionState.isReady }

    func connect() {
        userRequestedConnection = true
        reconnectAttempt = 0
        connectIfPossible()
    }

    func connect(to deviceID: UUID) {
        guard let device = discoveredPeripherals[deviceID] else { return }
        userRequestedConnection = true
        reconnectAttempt = 0
        central.stopScan()
        cancelPhaseTimeout()
        activePeripheral = device
        peripheralName = device.name ?? "SMART_BOXING_GLOVE"
        connectIfPossible()
    }

    func disconnect() {
        userRequestedConnection = false
        reconnectWorkItem?.cancel()
        cancelPhaseTimeout()
        central.stopScan()
        if let activePeripheral, activePeripheral.state != .disconnected {
            central.cancelPeripheralConnection(activePeripheral)
        }
        clearReceiveState(keepingCharacteristic: false)
        activePeripheral = nil
        connectionState = .idle
    }

    /// The MVP is foreground-only. The view model pauses the training before this
    /// transport is stopped, so no background collection is represented to the person.
    func setForegroundAllowed(_ allowed: Bool) {
        foregroundAllowed = allowed
        guard !allowed else {
            if userRequestedConnection { connectIfPossible() }
            return
        }

        reconnectWorkItem?.cancel()
        cancelPhaseTimeout()
        central.stopScan()
        if let activePeripheral, activePeripheral.state != .disconnected {
            central.cancelPeripheralConnection(activePeripheral)
        }
        clearReceiveState(keepingCharacteristic: false)
        connectionState = .idle
    }

    private func connectIfPossible() {
        guard foregroundAllowed, userRequestedConnection else { return }
        guard central.state == .poweredOn else {
            connectionState = .bluetoothUnavailable(bluetoothUnavailableMessage)
            return
        }

        guard let activePeripheral else {
            beginScan()
            return
        }

        switch activePeripheral.state {
        case .connected:
            discoverGloveService(on: activePeripheral)
        case .connecting:
            break
        case .disconnected:
            beginConnection(to: activePeripheral)
        case .disconnecting:
            break
        @unknown default:
            beginScan()
        }
    }

    private func beginScan() {
        guard foregroundAllowed, userRequestedConnection, central.state == .poweredOn else { return }
        activePeripheral = nil
        discoveredPeripherals.removeAll(keepingCapacity: false)
        discoveredDevices.removeAll(keepingCapacity: false)
        central.stopScan()
        connectionState = .scanning
        schedulePhaseTimeout(after: 15, expectedState: .scanning) { [weak self] in
            guard let self else { return }
            self.central.stopScan()
            self.connectionState = .failed("Não foi encontrada nenhuma luva. Tente novamente.")
        }
        central.scanForPeripherals(
            withServices: [serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    private func beginConnection(to peripheral: CBPeripheral) {
        guard foregroundAllowed, userRequestedConnection, central.state == .poweredOn else { return }
        activePeripheral = peripheral
        peripheralName = peripheral.name ?? "SMART_BOXING_GLOVE"
        peripheral.delegate = self
        connectionState = .connecting(peripheralName)
        schedulePhaseTimeout(after: 12, expectedState: .connecting(peripheralName)) { [weak self, weak peripheral] in
            guard let self, let peripheral, self.isActive(peripheral) else { return }
            self.fail("A ligação à luva excedeu o tempo de espera.", cancelConnection: true)
        }
        central.connect(peripheral)
    }

    private var bluetoothUnavailableMessage: String {
        switch central.state {
        case .poweredOff: return "Ative o Bluetooth para ligar a luva"
        case .unauthorized: return "Autorize o Bluetooth nas Definições"
        case .unsupported: return "Este iPhone não suporta Bluetooth LE"
        case .resetting: return "Bluetooth a reiniciar"
        case .unknown: return "A verificar Bluetooth"
        case .poweredOn: return "Bluetooth pronto"
        @unknown default: return "Bluetooth indisponível"
        }
    }

    private func discoverGloveService(on peripheral: CBPeripheral) {
        guard isActive(peripheral), foregroundAllowed, userRequestedConnection else { return }
        cancelPhaseTimeout()
        peripheral.delegate = self
        connectionState = .subscribing(peripheralName)
        schedulePhaseTimeout(after: 10, expectedState: .subscribing(peripheralName)) { [weak self, weak peripheral] in
            guard let self, let peripheral, self.isActive(peripheral) else { return }
            self.fail("A luva demorou demasiado a preparar a ligação.", cancelConnection: true)
        }
        peripheral.discoverServices([serviceUUID])
    }

    private func startHandshake(on peripheral: CBPeripheral, characteristic: CBCharacteristic) {
        guard isActive(peripheral), foregroundAllowed, userRequestedConnection else { return }
        // The protocol requires clearing sequence, session, deduplication, and stats
        // immediately before every HELLO because HELLO begins a new device session.
        clearReceiveState(keepingCharacteristic: true)
        handshake = .awaitingAcknowledgement
        connectionState = .handshaking(peripheralName)
        schedulePhaseTimeout(after: 8, expectedState: .handshaking(peripheralName)) { [weak self, weak peripheral] in
            guard let self, let peripheral, self.isActive(peripheral) else { return }
            self.fail("A luva não confirmou a preparação a tempo.", cancelConnection: true)
        }
        peripheral.writeValue(GloveProtocol.hello, for: characteristic, type: .withResponse)
    }

    private func clearReceiveState(keepingCharacteristic: Bool) {
        handshake = .idle
        activeSessionID = nil
        lastSequence = nil
        recentPunchIDs.removeAll(keepingCapacity: false)
        punchIDOrder.removeAll(keepingCapacity: false)
        timestampUnwrapper.reset()
        latestStats.removeAll(keepingCapacity: false)
        statsChunkTotal = nil
        missedTransportFrames = 0
        if !keepingCharacteristic { dataCharacteristic = nil }
    }

    private func scheduleReconnect() {
        guard foregroundAllowed, userRequestedConnection, central.state == .poweredOn, activePeripheral != nil else { return }
        reconnectWorkItem?.cancel()
        reconnectAttempt = min(reconnectAttempt + 1, 5)
        let delay = min(8.0, 0.8 * pow(1.7, Double(reconnectAttempt - 1)))
        connectionState = .reconnecting
        let work = DispatchWorkItem { [weak self] in self?.connectIfPossible() }
        reconnectWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func schedulePhaseTimeout(
        after delay: TimeInterval,
        expectedState: GloveConnectionState,
        action: @escaping () -> Void
    ) {
        cancelPhaseTimeout()
        let token = UUID()
        phaseToken = token
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.phaseToken == token, self.connectionState == expectedState else { return }
            action()
        }
        phaseTimeout = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func cancelPhaseTimeout() {
        phaseToken = UUID()
        phaseTimeout?.cancel()
        phaseTimeout = nil
    }

    private func fail(_ message: String, cancelConnection: Bool) {
        let wasReady = handshake == .ready
        cancelPhaseTimeout()
        clearReceiveState(keepingCharacteristic: false)
        lastProtocolIssue = message
        connectionState = .failed(message)
        if wasReady {
            onConnectionLost?("A ligação à luva foi interrompida; o treino foi colocado em pausa.")
        }
        if cancelConnection, let activePeripheral {
            central.cancelPeripheralConnection(activePeripheral)
        }
    }

    private func isActive(_ peripheral: CBPeripheral) -> Bool {
        activePeripheral?.identifier == peripheral.identifier
    }

    private func isCurrentTransportCallback(_ peripheral: CBPeripheral) -> Bool {
        isActive(peripheral) && foregroundAllowed && userRequestedConnection
    }

    private func handleIncoming(_ data: Data) {
        do {
            let frame = try GloveProtocol.parseFrame(data)
            if frame.type == GloveProtocol.MessageType.helloAck.rawValue {
                try acceptHelloAcknowledgement(frame)
                return
            }

            guard handshake == .ready, frame.sessionID == activeSessionID else {
                lastProtocolIssue = "Notificação fora da sessão atual ignorada."
                return
            }

            switch GloveProtocol.sequenceCheck(previous: lastSequence, incoming: frame.sequence) {
            case .duplicate, .stale:
                return
            case let .accepted(missedFrames):
                switch frame.type {
                case GloveProtocol.MessageType.event.rawValue:
                    let event = try GloveProtocol.parseEvent(frame)
                    acceptSequence(frame.sequence, missedFrames: missedFrames)
                    guard rememberPunchID(event.punchID) else { return }
                    let signal = GlovePunchSignal(
                        punchID: event.punchID,
                        triggerTimeMicros: event.triggerTimeMicros,
                        elapsedDeviceMicros: timestampUnwrapper.accept(event.triggerTimeMicros),
                        fsrADC: event.fsrADC,
                        receivedAt: Date()
                    )
                    onPunch?(signal)

                case GloveProtocol.MessageType.stats.rawValue:
                    let stats = try GloveProtocol.parseStats(frame)
                    acceptSequence(frame.sequence, missedFrames: missedFrames)
                    if statsChunkTotal != stats.total {
                        latestStats.removeAll(keepingCapacity: false)
                        statsChunkTotal = stats.total
                    }
                    latestStats[stats.index] = stats.values

                case GloveProtocol.MessageType.error.rawValue:
                    acceptSequence(frame.sequence, missedFrames: missedFrames)
                    lastProtocolIssue = "A luva comunicou um diagnóstico sem detalhes."

                default:
                    // Capture and unknown frames are never interpreted by this profile.
                    // Advancing the sequence avoids falsely reporting them as missing later.
                    acceptSequence(frame.sequence, missedFrames: missedFrames)
                    lastProtocolIssue = "Tipo de mensagem não solicitado ignorado."
                }
            }
        } catch {
            lastProtocolIssue = "Notificação v2 inválida ignorada."
        }
    }

    private func acceptHelloAcknowledgement(_ frame: GloveProtocol.Frame) throws {
        guard handshake == .awaitingAcknowledgement, frame.sequence == 0 else {
            throw GloveProtocol.Error.invalidHelloAcknowledgement
        }
        let acknowledgement = try GloveProtocol.parseHelloAcknowledgement(frame)
        cancelPhaseTimeout()
        activeSessionID = frame.sessionID
        lastSequence = frame.sequence
        reconnectAttempt = 0
        handshake = .ready
        connectionState = .ready(
            GloveDeviceInfo(
                name: peripheralName,
                accelerometerRateHz: acknowledgement.accelerometerRateHz,
                magnetometerRateHz: acknowledgement.magnetometerRateHz
            )
        )
    }

    private func acceptSequence(_ sequence: UInt16, missedFrames: Int) {
        lastSequence = sequence
        if missedFrames > 0 {
            missedTransportFrames &+= UInt32(min(missedFrames, Int(UInt32.max)))
        }
    }

    private func rememberPunchID(_ id: UInt16) -> Bool {
        guard !recentPunchIDs.contains(id) else { return false }
        recentPunchIDs.insert(id)
        punchIDOrder.append(id)
        if punchIDOrder.count > 256 {
            recentPunchIDs.remove(punchIDOrder.removeFirst())
        }
        return true
    }
}

extension GloveBluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            let wasReady = handshake == .ready
            cancelPhaseTimeout()
            clearReceiveState(keepingCharacteristic: false)
            connectionState = .bluetoothUnavailable(bluetoothUnavailableMessage)
            if wasReady {
                onConnectionLost?("A ligação Bluetooth foi interrompida; o treino foi colocado em pausa.")
            }
            return
        }
        if userRequestedConnection, foregroundAllowed { connectIfPossible() }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard foregroundAllowed, userRequestedConnection, connectionState == .scanning else { return }
        let name = peripheral.name
            ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? "SMART_BOXING_GLOVE"
        discoveredPeripherals[peripheral.identifier] = peripheral
        let discovered = GloveDiscoveredDevice(id: peripheral.identifier, name: name, rssi: RSSI.intValue)
        discoveredDevices.removeAll { $0.id == discovered.id }
        discoveredDevices.append(discovered)
        discoveredDevices.sort { lhs, rhs in
            lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard isCurrentTransportCallback(peripheral) else {
            central.cancelPeripheralConnection(peripheral)
            return
        }
        discoverGloveService(on: peripheral)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard isCurrentTransportCallback(peripheral) else { return }
        cancelPhaseTimeout()
        clearReceiveState(keepingCharacteristic: false)
        lastProtocolIssue = "Não foi possível ligar à luva selecionada."
        scheduleReconnect()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard isCurrentTransportCallback(peripheral) else { return }
        let wasReady = handshake == .ready
        cancelPhaseTimeout()
        clearReceiveState(keepingCharacteristic: false)
        if wasReady {
            onConnectionLost?("A luva desligou; o treino foi colocado em pausa.")
        }
        scheduleReconnect()
    }
}

extension GloveBluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard isCurrentTransportCallback(peripheral) else { return }
        guard error == nil,
              let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            fail("Serviço da luva não encontrado.", cancelConnection: true)
            return
        }
        peripheral.discoverCharacteristics([characteristicUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard isCurrentTransportCallback(peripheral) else { return }
        guard error == nil,
              let characteristic = service.characteristics?.first(where: { $0.uuid == characteristicUUID }),
              characteristic.properties.contains(.notify),
              characteristic.properties.contains(.write) else {
            fail("Característica da luva incompatível.", cancelConnection: true)
            return
        }
        dataCharacteristic = characteristic
        connectionState = .subscribing(peripheralName)
        peripheral.setNotifyValue(true, for: characteristic)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrentTransportCallback(peripheral), characteristic.uuid == characteristicUUID else { return }
        guard error == nil, characteristic.isNotifying else {
            fail("Não foi possível receber notificações da luva.", cancelConnection: true)
            return
        }
        guard let dataCharacteristic else {
            fail("Característica da luva indisponível.", cancelConnection: true)
            return
        }

        switch handshake {
        case .idle:
            startHandshake(on: peripheral, characteristic: dataCharacteristic)
        case .awaitingAcknowledgement:
            // Delegate delivery can repeat; do not send a second HELLO.
            return
        case .ready:
            onConnectionLost?("A subscrição da luva foi renovada; o treino foi colocado em pausa.")
            startHandshake(on: peripheral, characteristic: dataCharacteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrentTransportCallback(peripheral), characteristic.uuid == characteristicUUID else { return }
        if error != nil, handshake == .awaitingAcknowledgement {
            fail("A luva recusou a preparação da ligação.", cancelConnection: true)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard isCurrentTransportCallback(peripheral), characteristic.uuid == characteristicUUID else { return }
        guard error == nil, let data = characteristic.value else {
            fail("Não foi possível receber dados da luva.", cancelConnection: true)
            return
        }
        handleIncoming(data)
    }

    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        guard isCurrentTransportCallback(peripheral) else { return }
        let wasReady = handshake == .ready
        cancelPhaseTimeout()
        clearReceiveState(keepingCharacteristic: false)
        if wasReady {
            onConnectionLost?("Os serviços da luva mudaram; o treino foi colocado em pausa.")
        }
        discoverGloveService(on: peripheral)
    }
}
