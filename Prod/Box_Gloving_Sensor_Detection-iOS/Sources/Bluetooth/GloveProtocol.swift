import Foundation

/// Pure decoder for `docs/PROTOCOL_V2.md` in the sibling firmware project.
/// Every multi-byte field is read explicitly as little endian.
enum GloveProtocol {
    static let magic: UInt8 = 0xB2
    static let version: UInt8 = 2
    static let headerLength = 6
    static let serviceUUID = "12345678-1234-5678-1234-56789ABCDEF0"
    static let dataCharacteristicUUID = "12345678-1234-5678-1234-56789ABCDEF1"
    static let requestedCapabilities: UInt8 = 0x05 // events + diagnostics statistics
    static let maximumFSRADC: UInt16 = 4_095

    enum MessageType: UInt8 {
        case helloAck = 0x01
        case event = 0x10
        case captureInfo = 0x11
        case captureData = 0x12
        case stats = 0x13
        case error = 0x7F
    }

    struct Frame: Equatable {
        let type: UInt8
        let sessionID: UInt16
        let sequence: UInt16
        let payload: [UInt8]
    }

    struct HelloAcknowledgement: Equatable {
        let capabilities: UInt8
        let accelerometerRateHz: UInt16
        let magnetometerRateHz: UInt16
    }

    struct Event: Equatable {
        let punchID: UInt16
        let triggerTimeMicros: UInt32
        let fsrADC: UInt16
        let flags: UInt16
    }

    struct StatsChunk: Equatable {
        let index: UInt8
        let total: UInt8
        let values: [UInt32]

        var firstCounterIndex: Int { Int(index) * 3 }
    }

    enum Error: Swift.Error, Equatable {
        case invalidFrame
        case invalidHelloAcknowledgement
        case invalidEvent
        case invalidStats
    }

    static var hello: Data {
        Data([0x47, 0x42, version, requestedCapabilities]) // GB, v2, events + stats
    }

    static func parseFrame(_ data: Data) throws -> Frame {
        let bytes = [UInt8](data)
        guard bytes.count >= headerLength, bytes[0] == magic else {
            throw Error.invalidFrame
        }
        return Frame(
            type: bytes[1],
            sessionID: readUInt16(bytes, at: 2),
            sequence: readUInt16(bytes, at: 4),
            payload: Array(bytes.dropFirst(headerLength))
        )
    }

    static func parseHelloAcknowledgement(_ frame: Frame) throws -> HelloAcknowledgement {
        guard frame.type == MessageType.helloAck.rawValue,
              frame.payload.count == 6,
              frame.payload[0] == version,
              frame.payload[1] == requestedCapabilities else {
            throw Error.invalidHelloAcknowledgement
        }
        let accelRate = readUInt16(frame.payload, at: 2)
        let magnetometerRate = readUInt16(frame.payload, at: 4)
        guard accelRate > 0, magnetometerRate > 0 else {
            throw Error.invalidHelloAcknowledgement
        }
        return HelloAcknowledgement(
            capabilities: frame.payload[1],
            accelerometerRateHz: accelRate,
            magnetometerRateHz: magnetometerRate
        )
    }

    static func parseEvent(_ frame: Frame) throws -> Event {
        guard frame.type == MessageType.event.rawValue, frame.payload.count == 12 else {
            throw Error.invalidEvent
        }
        let fsrADC = readUInt16(frame.payload, at: 6)
        let flags = readUInt16(frame.payload, at: 10)
        // The force-estimate field at offsets 8...9 is intentionally not decoded
        // for presentation. It is not a calibrated physical measurement.
        guard fsrADC <= maximumFSRADC,
              flags & ~UInt16(0x0007) == 0,
              flags & 0x0001 != 0 else {
            throw Error.invalidEvent
        }
        return Event(
            punchID: readUInt16(frame.payload, at: 0),
            triggerTimeMicros: readUInt32(frame.payload, at: 2),
            fsrADC: fsrADC,
            flags: flags
        )
    }

    static func parseStats(_ frame: Frame) throws -> StatsChunk {
        guard frame.type == MessageType.stats.rawValue,
              frame.payload.count >= 6,
              frame.payload.count <= 14,
              (frame.payload.count - 2).isMultiple(of: 4) else {
            throw Error.invalidStats
        }
        let index = frame.payload[0]
        let total = frame.payload[1]
        // Eight chunks carry the firmware's 23 current counters. Keeping the
        // count field authoritative lets a later protocol extension add chunks.
        guard total > 0, total <= 64, index < total else {
            throw Error.invalidStats
        }
        let values = stride(from: 2, to: frame.payload.count, by: 4).map {
            readUInt32(frame.payload, at: $0)
        }
        guard !values.isEmpty, values.count <= 3 else { throw Error.invalidStats }
        if index < total - 1, values.count != 3 {
            throw Error.invalidStats
        }
        return StatsChunk(index: index, total: total, values: values)
    }

    static func sequenceCheck(previous: UInt16?, incoming: UInt16) -> SequenceCheck {
        guard let previous else { return .accepted(missedFrames: 0) }
        let delta = incoming &- previous
        if delta == 0 { return .duplicate }
        if delta < 0x8000 { return .accepted(missedFrames: Int(delta - 1)) }
        return .stale
    }

    private static func readUInt16(_ bytes: [UInt8], at index: Int) -> UInt16 {
        UInt16(bytes[index]) | (UInt16(bytes[index + 1]) << 8)
    }

    private static func readUInt32(_ bytes: [UInt8], at index: Int) -> UInt32 {
        UInt32(bytes[index]) |
            (UInt32(bytes[index + 1]) << 8) |
            (UInt32(bytes[index + 2]) << 16) |
            (UInt32(bytes[index + 3]) << 24)
    }
}

enum SequenceCheck: Equatable {
    case accepted(missedFrames: Int)
    case duplicate
    case stale
}

/// Unwraps the device's 32-bit uptime without making it a date or latency claim.
struct DeviceTimestampUnwrapper {
    private(set) var lastRaw: UInt32?
    private(set) var elapsedMicros: UInt64?

    mutating func accept(_ raw: UInt32) -> UInt64? {
        guard let lastRaw, let elapsedMicros else {
            self.lastRaw = raw
            self.elapsedMicros = 0
            return 0
        }
        let delta = raw &- lastRaw
        guard delta < 0x8000_0000 else { return nil }
        self.lastRaw = raw
        self.elapsedMicros = elapsedMicros &+ UInt64(delta)
        return self.elapsedMicros
    }

    mutating func reset() {
        lastRaw = nil
        elapsedMicros = nil
    }
}
