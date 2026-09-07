import SwiftUI

struct ProgressDashboardView: View {
    @EnvironmentObject private var model: TrainingViewModel

    private var progress: ProgressSummary { model.progression }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    intro
                    dailyGoal
                    totals
                    achievements
                    transparencyNote
                }
                .padding(16)
                .padding(.bottom, 18)
            }
            .background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("Progresso")
        }
    }

    private var intro: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text("O seu ritmo")
                .font(.system(.largeTitle, design: .rounded, weight: .bold))
            Text("Metas e conquistas usam apenas sessões reais concluídas. Demonstrações ficam fora destes valores.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .padding(.top, 6)
    }

    private var dailyGoal: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Meta de hoje")
                            .font(.headline)
                        Text("\(progress.punchesToday) de \(progress.dailyGoal) eventos reais")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text("\(Int((progress.dailyProgress * 100).rounded()))%")
                        .font(.title3.weight(.bold))
                        .foregroundStyle(AppPalette.accent)
                        .monospacedDigit()
                }
                SwiftUI.ProgressView(value: progress.dailyProgress)
                    .tint(AppPalette.accent)
                    .accessibilityLabel("Progresso da meta diária")
                    .accessibilityValue("\(progress.punchesToday) de \(progress.dailyGoal)")
            }
        }
    }

    private var totals: some View {
        HStack(spacing: 10) {
            MetricTile(
                title: "Eventos reais",
                value: "\(progress.totalRealPunches)",
                detail: "Em sessões concluídas",
                symbol: "circlebadge.fill"
            )
            MetricTile(
                title: "Sessões reais",
                value: "\(progress.completedRealSessions)",
                detail: "Com eventos registados",
                symbol: "calendar.badge.checkmark",
                tint: .green
            )
        }
    }

    private var achievements: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("Conquistas", subtitle: "Regras locais e transparentes.")
            ForEach(progress.achievements) { achievement in
                SurfaceCard {
                    HStack(spacing: 14) {
                        Image(systemName: achievement.symbol)
                            .font(.title3)
                            .foregroundStyle(achievement.completed ? .yellow : .secondary)
                            .frame(width: 28)
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(achievement.title)
                                .font(.subheadline.weight(.bold))
                            Text(achievement.detail)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Text("\(min(achievement.currentValue, achievement.targetValue))/\(achievement.targetValue)")
                            .font(.caption.weight(.bold))
                            .foregroundStyle(achievement.completed ? .green : .secondary)
                            .monospacedDigit()
                    }
                }
                .accessibilityElement(children: .combine)
            }
        }
    }

    private var transparencyNote: some View {
        Label("Uma conquista reflete eventos guardados, não a qualidade técnica nem a intensidade física do golpe.", systemImage: "info.circle")
            .font(.caption)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 4)
    }
}
