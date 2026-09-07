import Foundation

enum StoreLoadState: Equatable {
    case ready
    case recoveredFromBackup
    case recoveryRequired(String)
    case unavailable(String)

    var permitsWrites: Bool {
        switch self {
        case .ready, .recoveredFromBackup: return true
        case .recoveryRequired, .unavailable: return false
        }
    }

    var userMessage: String? {
        switch self {
        case .ready: return nil
        case .recoveredFromBackup:
            return "Recuperámos os dados locais a partir de uma cópia segura."
        case let .recoveryRequired(message), let .unavailable(message):
            return message
        }
    }
}

struct StoreLoadResult {
    let snapshot: AppSnapshot
    let state: StoreLoadState

    var recoveredFromBackup: Bool {
        if case .recoveredFromBackup = state { return true }
        return false
    }
}

enum LocalStoreError: LocalizedError {
    case unsupportedSchema(Int)

    var errorDescription: String? {
        switch self {
        case let .unsupportedSchema(version):
            return "Versão de dados local não suportada: \(version)."
        }
    }
}

/// JSON persistence with an atomic primary and a last-known-good backup.
/// Corrupt or incompatible data is never replaced implicitly: `load()` places the
/// caller in an explicit read-only recovery state until the person resets it.
final class LocalStore {
    private enum InspectedFile {
        case missing
        case snapshot(AppSnapshot)
        case unreadable(Error)
    }

    private let fileManager: FileManager
    private let directoryURL: URL
    private let stateURL: URL
    private let backupURL: URL

    init(fileManager: FileManager = .default, directoryURL: URL? = nil) {
        self.fileManager = fileManager
        let baseDirectory = directoryURL ?? fileManager.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first!.appendingPathComponent("SmartBoxingGlove", isDirectory: true)
        self.directoryURL = baseDirectory
        stateURL = baseDirectory.appendingPathComponent("training-state.json")
        backupURL = baseDirectory.appendingPathComponent("training-state.backup.json")
    }

    func load() -> StoreLoadResult {
        do {
            try ensureDirectory()
        } catch {
            return StoreLoadResult(
                snapshot: .empty(),
                state: .unavailable("Não foi possível aceder ao armazenamento local. Os dados não serão alterados.")
            )
        }

        let primary = inspect(stateURL)
        let backup = inspect(backupURL)

        switch primary {
        case let .snapshot(snapshot):
            return StoreLoadResult(snapshot: snapshot, state: .ready)

        case .missing:
            switch backup {
            case let .snapshot(snapshot):
                return StoreLoadResult(snapshot: snapshot, state: .recoveredFromBackup)
            case .missing:
                return StoreLoadResult(snapshot: .empty(), state: .ready)
            case let .unreadable(error):
                return recoveryResult(primaryError: nil, backupError: error)
            }

        case let .unreadable(primaryError):
            switch backup {
            case let .snapshot(snapshot):
                return StoreLoadResult(snapshot: snapshot, state: .recoveredFromBackup)
            case .missing:
                return recoveryResult(primaryError: primaryError, backupError: nil)
            case let .unreadable(backupError):
                return recoveryResult(primaryError: primaryError, backupError: backupError)
            }
        }
    }

    func save(_ snapshot: AppSnapshot) throws {
        try ensureDirectory()
        let data = try encoder.encode(snapshot)

        // A malformed or newer primary must never overwrite the recovery copy.
        if case let .snapshot(existingSnapshot) = inspect(stateURL) {
            let existingData = try encoder.encode(existingSnapshot)
            try existingData.write(to: backupURL, options: .atomic)
        }
        try data.write(to: stateURL, options: .atomic)
    }

    func exportData(for snapshot: AppSnapshot) throws -> Data {
        try encoder.encode(snapshot)
    }

    /// This is intentionally the only operation that removes recovery files.
    func deleteAll() throws {
        for fileURL in [stateURL, backupURL] where fileManager.fileExists(atPath: fileURL.path) {
            try fileManager.removeItem(at: fileURL)
        }
    }

    private func recoveryResult(primaryError: Error?, backupError: Error?) -> StoreLoadResult {
        let detail: String
        if primaryError != nil, backupError != nil {
            detail = "O histórico local e a cópia de segurança não podem ser lidos. Reveja ou exporte os ficheiros fora da app antes de apagar os dados."
        } else {
            detail = "O histórico local não pode ser lido. A app está em modo de recuperação e não alterará os ficheiros até escolher apagar os dados locais."
        }
        return StoreLoadResult(snapshot: .empty(), state: .recoveryRequired(detail))
    }

    private func ensureDirectory() throws {
        try fileManager.createDirectory(at: directoryURL, withIntermediateDirectories: true)
    }

    private func inspect(_ url: URL) -> InspectedFile {
        guard fileManager.fileExists(atPath: url.path) else { return .missing }
        do {
            let data = try Data(contentsOf: url)
            let snapshot = try decode(data)
            guard snapshot.schemaVersion == AppSnapshot.currentSchemaVersion else {
                throw LocalStoreError.unsupportedSchema(snapshot.schemaVersion)
            }
            return .snapshot(snapshot)
        } catch {
            return .unreadable(error)
        }
    }

    private func decode(_ data: Data) throws -> AppSnapshot {
        do {
            return try modernDecoder.decode(AppSnapshot.self, from: data)
        } catch {
            // Existing development builds wrote whole-second ISO-8601 dates.
            // Preserve that data while all new writes retain milliseconds.
            return try legacyDecoder.decode(AppSnapshot.self, from: data)
        }
    }

    private var encoder: JSONEncoder {
        let result = JSONEncoder()
        result.dateEncodingStrategy = .millisecondsSince1970
        result.outputFormatting = [.sortedKeys]
        return result
    }

    private var modernDecoder: JSONDecoder {
        let result = JSONDecoder()
        result.dateDecodingStrategy = .millisecondsSince1970
        return result
    }

    private var legacyDecoder: JSONDecoder {
        let result = JSONDecoder()
        result.dateDecodingStrategy = .iso8601
        return result
    }
}

/// Serializes disk work away from the BLE/main queue. Punch checkpoints are coalesced;
/// pause, finish, settings, and recovery changes request an immediate ordered flush.
final class SnapshotWriter {
    enum Urgency {
        case coalesced
        case immediate
    }

    var onFailure: ((Error) -> Void)?

    private let store: LocalStore
    private let queue = DispatchQueue(label: "pt.smartboxingglove.snapshot-writer", qos: .utility)
    private var delayedWrite: DispatchWorkItem?
    private var pendingSnapshot: AppSnapshot?

    init(store: LocalStore) {
        self.store = store
    }

    func persist(_ snapshot: AppSnapshot, urgency: Urgency, completion: ((Result<Void, Error>) -> Void)? = nil) {
        queue.async { [weak self] in
            guard let self else { return }
            switch urgency {
            case .coalesced:
                self.pendingSnapshot = snapshot
                self.delayedWrite?.cancel()
                let write = DispatchWorkItem { [weak self] in self?.writePending() }
                self.delayedWrite = write
                self.queue.asyncAfter(deadline: .now() + 0.75, execute: write)

            case .immediate:
                self.delayedWrite?.cancel()
                self.delayedWrite = nil
                self.pendingSnapshot = nil
                self.write(snapshot, completion: completion)
            }
        }
    }

    func deleteAll(completion: @escaping (Result<Void, Error>) -> Void) {
        queue.async { [weak self] in
            guard let self else { return }
            self.delayedWrite?.cancel()
            self.delayedWrite = nil
            self.pendingSnapshot = nil
            let result = Result { try self.store.deleteAll() }
            DispatchQueue.main.async { completion(result) }
        }
    }

    private func writePending() {
        delayedWrite = nil
        guard let snapshot = pendingSnapshot else { return }
        pendingSnapshot = nil
        write(snapshot, completion: nil)
    }

    private func write(_ snapshot: AppSnapshot, completion: ((Result<Void, Error>) -> Void)?) {
        let result = Result { try store.save(snapshot) }
        DispatchQueue.main.async { [weak self] in
            if case let .failure(error) = result {
                self?.onFailure?(error)
            }
            completion?(result)
        }
    }
}
