import SwiftUI

struct HistoryView: View {
    @EnvironmentObject private var model: TrainingViewModel
    @State private var source: TrainingSource = .glove

    private var sessions: [TrainingSession] {
        source == .glove ? model.realSessions : model.demoSessions
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Sessões guardadas neste iPhone")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        Picker("Tipo de sessão", selection: $source) {
                            ForEach(TrainingSource.allCases) { option in
                                Text(option.title).tag(option)
                            }
                        }
                        .pickerStyle(.segmented)
                    }

                    if source == .demo {
                        SurfaceCard {
                            Label("Histórico de demonstração — estes registos não fazem parte do progresso real.", systemImage: "theatermasks.fill")
                                .font(.footnote.weight(.medium))
                                .foregroundStyle(.orange)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }

                    if sessions.isEmpty {
                        EmptyStateView(
                            symbol: source == .glove ? "clock.badge.xmark" : "theatermasks",
                            title: source == .glove ? "Ainda não há treinos reais" : "Ainda não há demonstrações",
                            detail: source == .glove
                                ? "Conclua um treino com a luva para o ver aqui."
                                : "Inicie o modo demonstração para explorar a interface."
                        )
                    } else {
                        LazyVStack(spacing: 12) {
                            ForEach(sessions) { session in
                                SessionHistoryCard(session: session)
                            }
                        }
                    }
                }
                .padding(16)
                .padding(.bottom, 18)
            }
            .background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("Histórico")
        }
    }
}

private struct SessionHistoryCard: View {
    let session: TrainingSession

    var body: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(session.endedAt.formatted(date: .abbreviated, time: .shortened))
                            .font(.headline)
                        Text(session.source == .glove ? "Treino com a luva" : "Demonstração")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(session.source == .glove ? .secondary : .orange)
                    }
                    Spacer()
                    Image(systemName: session.source == .glove ? "sensor.tag.radiowaves.forward" : "theatermasks.fill")
                        .foregroundStyle(session.source == .glove ? AppPalette.accent : .orange)
                        .accessibilityHidden(true)
                }

                HStack(spacing: 10) {
                    compactMetric("Eventos", value: "\(session.punchCount)", symbol: "circlebadge.fill")
                    compactMetric("Duração ativa", value: session.duration.clockText, symbol: "timer")
                }
            }
        }
        .accessibilityElement(children: .combine)
    }

    private func compactMetric(_ title: String, value: String, symbol: String) -> some View {
        HStack(spacing: 7) {
            Image(systemName: symbol)
                .font(.caption)
                .foregroundStyle(AppPalette.accent)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 1) {
                Text(value)
                    .font(.subheadline.weight(.bold))
                    .monospacedDigit()
                Text(title)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(AppPalette.subtle, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}
