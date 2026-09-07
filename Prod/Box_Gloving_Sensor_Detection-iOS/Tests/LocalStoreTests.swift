import XCTest
@testable import SmartBoxingGlove

final class LocalStoreTests: XCTestCase {
    private var directory: URL!

    override func setUpWithError() throws {
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("SmartBoxingGloveTests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let directory, FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
    }

    func testMillisecondDatesSurviveRoundTrip() throws {
        let store = LocalStore(directoryURL: directory)
        let timestamp = Date(timeIntervalSince1970: 1_726_000_000.987)
        var snapshot = AppSnapshot.empty(now: timestamp)
        snapshot.realSessions = [session(at: timestamp)]
        try store.save(snapshot)

        let loaded = store.load()
        XCTAssertTrue(loaded.state.permitsWrites)
        let receivedAt = try XCTUnwrap(loaded.snapshot.realSessions.first?.punches.first?.receivedAt)
        XCTAssertEqual(receivedAt.timeIntervalSince1970, timestamp.timeIntervalSince1970, accuracy: 0.001)
    }

    func testValidBackupIsUsedWhenPrimaryBecomesUnreadable() throws {
        let store = LocalStore(directoryURL: directory)
        let first = AppSnapshot.empty(now: Date(timeIntervalSince1970: 1))
        var second = AppSnapshot.empty(now: Date(timeIntervalSince1970: 2))
        second.settings.dailyPunchGoal = 90
        try store.save(first)
        try store.save(second)
        try Data("not-json".utf8).write(to: directory.appendingPathComponent("training-state.json"), options: .atomic)

        let loaded = store.load()
        XCTAssertTrue(loaded.recoveredFromBackup)
        XCTAssertEqual(loaded.snapshot.settings.dailyPunchGoal, first.settings.dailyPunchGoal)
    }

    func testUnreadablePrimaryAndBackupRequireExplicitRecoveryAndKeepFiles() throws {
        let primary = directory.appendingPathComponent("training-state.json")
        let backup = directory.appendingPathComponent("training-state.backup.json")
        try Data("broken-primary".utf8).write(to: primary, options: .atomic)
        try Data("broken-backup".utf8).write(to: backup, options: .atomic)

        let loaded = LocalStore(directoryURL: directory).load()
        guard case .recoveryRequired = loaded.state else {
            return XCTFail("Expected explicit recovery state")
        }
        XCTAssertFalse(loaded.state.permitsWrites)
        XCTAssertTrue(FileManager.default.fileExists(atPath: primary.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: backup.path))
    }

    func testRecoveredRunningSessionDoesNotChargeDowntime() {
        var active = ActiveTrainingSession(source: .glove, now: Date(timeIntervalSince1970: 10))
        active.checkpoint(adding: 42, at: Date(timeIntervalSince1970: 52))
        active.pauseForRecovery(at: Date(timeIntervalSince1970: 86_452))

        XCTAssertEqual(active.state, .paused)
        XCTAssertEqual(active.accumulatedDuration, 42, accuracy: 0.0001)
        XCTAssertNil(active.resumedAt)
    }

    private func session(at date: Date) -> TrainingSession {
        var active = ActiveTrainingSession(source: .glove, now: date)
        active.punches = [PunchRecord(receivedAt: date, source: .glove)]
        active.pause(adding: 1, at: date.addingTimeInterval(1))
        return TrainingSession(from: active, endedAt: date.addingTimeInterval(1))
    }
}
