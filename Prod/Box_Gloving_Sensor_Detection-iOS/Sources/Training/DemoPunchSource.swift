import Foundation

struct DemoPunchSignal: Equatable {
    let id: UUID
    let receivedAt: Date
}

/// A deliberately isolated synthetic source for exploring the interface.
/// It does not have device identifiers, ADC data, or access to real progression.
final class DemoPunchSource {
    private var timer: Timer?
    private var emit: ((DemoPunchSignal) -> Void)?

    func start(emit: @escaping (DemoPunchSignal) -> Void) {
        stop()
        self.emit = emit
        scheduleNext(after: 0.55)
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        emit = nil
    }

    private func scheduleNext(after interval: TimeInterval) {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: interval, repeats: false) { [weak self] _ in
            guard let self else { return }
            self.emit?(DemoPunchSignal(id: UUID(), receivedAt: Date()))
            // A varied, intentionally modest visual rhythm. It never represents a sensor measurement.
            self.scheduleNext(after: [0.7, 0.9, 1.15, 1.4].randomElement() ?? 1.0)
        }
    }
}
