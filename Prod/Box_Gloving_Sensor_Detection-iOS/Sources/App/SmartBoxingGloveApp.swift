import SwiftUI

@main
struct SmartBoxingGloveApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var model = TrainingViewModel()

    var body: some Scene {
        WindowGroup {
            RootTabView()
                .environmentObject(model)
                .onAppear {
                    model.handleScenePhase(isActive: scenePhase == .active)
                }
                .onChange(of: scenePhase) { _, newPhase in
                    model.handleScenePhase(isActive: newPhase == .active)
                }
        }
    }
}
