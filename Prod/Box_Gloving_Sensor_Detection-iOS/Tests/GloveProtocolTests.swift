import XCTest
@testable import SmartBoxingGlove

final class GloveProtocolTests: XCTestCase {
    func testSharedProtocolFixtures() throws {
        let fixtures = try loadFixtures()
        XCTAssertEqual(GloveProtocol.hello, try Data(hex: fixtures.hello))

        for fixture in fixtures.validFrames {
            let frame = try GloveProtocol.parseFrame(Data(hex: fixture.hex))
            switch fixture.kind {
            case "helloAck":
                let acknowledgement = try GloveProtocol.parseHelloAcknowledgement(frame)
                XCTAssertEqual(acknowledgement.capabilities, 0x05, fixture.name)
                XCTAssertEqual(acknowledgement.accelerometerRateHz, 800, fixture.name)
                XCTAssertEqual(acknowledgement.magnetometerRateHz, 200, fixture.name)

            case "event":
                let event = try GloveProtocol.parseEvent(frame)
                XCTAssertEqual(event.fsrADC, try XCTUnwrap(fixture.adc), fixture.name)

            case "stats":
                let stats = try GloveProtocol.parseStats(frame)
                XCTAssertEqual(stats.index, try XCTUnwrap(fixture.index), fixture.name)
                XCTAssertEqual(stats.total, try XCTUnwrap(fixture.total), fixture.name)
                XCTAssertEqual(stats.values, try XCTUnwrap(fixture.values), fixture.name)

            default:
                XCTFail("Unknown shared fixture kind: \(fixture.kind)")
            }
        }
    }

    func testSharedMalformedFixturesAreRejectedAtTheirDeclaredStage() throws {
        let fixtures = try loadFixtures()
        for fixture in fixtures.invalidFrames {
            let data = try Data(hex: fixture.hex)
            switch fixture.stage {
            case "frame":
                XCTAssertThrowsError(try GloveProtocol.parseFrame(data), fixture.name)
            case "event":
                let frame = try GloveProtocol.parseFrame(data)
                XCTAssertThrowsError(try GloveProtocol.parseEvent(frame), fixture.name)
            case "stats":
                let frame = try GloveProtocol.parseFrame(data)
                XCTAssertThrowsError(try GloveProtocol.parseStats(frame), fixture.name)
            default:
                XCTFail("Unknown invalid fixture stage: \(fixture.stage)")
            }
        }
    }

    func testSharedSequenceFixturesHandleWrapDuplicateAndStaleFrames() throws {
        let fixtures = try loadFixtures()
        for fixture in fixtures.sequenceCases {
            let result = GloveProtocol.sequenceCheck(previous: fixture.previous, incoming: fixture.incoming)
            switch (fixture.outcome, result) {
            case let ("accepted", .accepted(missedFrames: missed)):
                XCTAssertEqual(missed, try XCTUnwrap(fixture.missed), fixture.outcome)
            case ("duplicate", .duplicate), ("stale", .stale):
                break
            default:
                XCTFail("Unexpected sequence result for \(fixture.outcome)")
            }
        }
    }

    func testEventAcceptsMaximumTwelveBitADC() throws {
        let frame = try GloveProtocol.parseFrame(Data(hex: "B21034120100420004030201FF0F00000100"))
        XCTAssertEqual(try GloveProtocol.parseEvent(frame).fsrADC, 4_095)
    }

    func testTimestampUnwrapperAcceptsCounterWrapWithoutCreatingWallTime() {
        var unwrapper = DeviceTimestampUnwrapper()
        XCTAssertEqual(unwrapper.accept(0xFFFF_FFF0), 0)
        XCTAssertEqual(unwrapper.accept(0x0000_0010), 32)
        XCTAssertNil(unwrapper.accept(0x0000_000F))
    }
}

private struct ProtocolFixtureSet: Decodable {
    let hello: String
    let validFrames: [ProtocolFixture]
    let invalidFrames: [InvalidProtocolFixture]
    let sequenceCases: [SequenceFixture]
}

private struct ProtocolFixture: Decodable {
    let name: String
    let kind: String
    let hex: String
    let adc: UInt16?
    let index: UInt8?
    let total: UInt8?
    let values: [UInt32]?
}

private struct InvalidProtocolFixture: Decodable {
    let name: String
    let stage: String
    let hex: String
}

private struct SequenceFixture: Decodable {
    let previous: UInt16
    let incoming: UInt16
    let outcome: String
    let missed: Int?
}

private extension GloveProtocolTests {
    func loadFixtures() throws -> ProtocolFixtureSet {
        let bundle = Bundle(for: type(of: self))
        let url = try XCTUnwrap(bundle.url(forResource: "protocol-v2-fixtures", withExtension: "json"))
        return try JSONDecoder().decode(ProtocolFixtureSet.self, from: Data(contentsOf: url))
    }
}

private extension Data {
    init(hex: String) throws {
        guard hex.count.isMultiple(of: 2) else {
            throw FixtureError.invalidHex
        }
        let pairs = stride(from: 0, to: hex.count, by: 2).map { offset -> String in
            let start = hex.index(hex.startIndex, offsetBy: offset)
            let end = hex.index(start, offsetBy: 2)
            return String(hex[start..<end])
        }
        guard let bytes = try? pairs.map({ pair in
            guard let value = UInt8(pair, radix: 16) else { throw FixtureError.invalidHex }
            return value
        }) else {
            throw FixtureError.invalidHex
        }
        self.init(bytes)
    }
}

private enum FixtureError: Error {
    case invalidHex
}
