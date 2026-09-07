import SwiftUI

struct RootTabView: View {
    @EnvironmentObject private var model: TrainingViewModel

    var body: some View {
        ZStack(alignment: .top) {
            TabView {
                TrainingDashboardView()
                    .tabItem { Label("Treino", systemImage: "figure.boxing") }

                HistoryView()
                    .tabItem { Label("Histórico", systemImage: "clock.arrow.circlepath") }

                ProgressDashboardView()
                    .tabItem { Label("Progresso", systemImage: "chart.bar.fill") }

                SettingsView()
                    .tabItem { Label("Definições", systemImage: "gearshape.fill") }
            }
            .tint(AppPalette.accent)

            if let message = model.storageNotice {
                NoticeBanner(message: message, tint: .orange) {
                    model.dismissStorageNotice()
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .transition(.move(edge: .top).combined(with: .opacity))
                .zIndex(1)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: model.storageNotice)
    }
}
