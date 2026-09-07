import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @EnvironmentObject private var model: TrainingViewModel
    @State private var exportDocument: ExportDocument?
    @State private var showingExporter = false
    @State private var confirmingDelete = false

    var body: some View {
        NavigationStack {
            List {
                trainingPreferences
                localData
                privacy
                about
            }
            .navigationTitle("Definições")
            .alert("Apagar dados locais?", isPresented: $confirmingDelete) {
                Button("Cancelar", role: .cancel) {}
                Button("Apagar", role: .destructive) {
                    model.deleteAllLocalData()
                }
            } message: {
                if model.needsRecoveryReset {
                    Text("Isto remove os ficheiros locais que a app não conseguiu ler e cria um novo histórico vazio.")
                } else {
                    Text("Esta ação remove sessões, demonstrações, metas e preferências guardadas neste iPhone.")
                }
            }
            .fileExporter(
                isPresented: $showingExporter,
                document: exportDocument,
                contentType: .json,
                defaultFilename: "smart-boxing-glove"
            ) { _ in }
        }
    }

    private var trainingPreferences: some View {
        Section {
            Toggle(
                "Feedback tátil ao registar evento",
                isOn: Binding(
                    get: { model.settings.hapticsEnabled },
                    set: { model.setHapticsEnabled($0) }
                )
            )
            .disabled(!model.canWriteLocalData)

            Stepper(
                value: Binding(
                    get: { model.settings.dailyPunchGoal },
                    set: { model.setDailyGoal($0) }
                ),
                in: 10...500,
                step: 10
            ) {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Meta diária")
                    Text("\(model.settings.dailyPunchGoal) eventos reais")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .disabled(!model.canWriteLocalData)
        } header: {
            Text("Treino")
        } footer: {
            Text("A meta usa apenas eventos guardados em sessões reais concluídas.")
        }
    }

    private var localData: some View {
        Section {
            if model.needsRecoveryReset {
                Label("Recuperação ativa: os ficheiros existentes serão preservados até escolher apagá-los.", systemImage: "lock.fill")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }

            Button {
                exportDocument = model.exportDocument()
                showingExporter = exportDocument != nil
            } label: {
                Label("Exportar dados locais", systemImage: "square.and.arrow.up")
            }
            .disabled(!model.canWriteLocalData)

            Button(role: .destructive) {
                confirmingDelete = true
            } label: {
                Label(
                    model.needsRecoveryReset ? "Apagar ficheiros e recomeçar" : "Apagar todos os dados locais",
                    systemImage: "trash"
                )
            }
        } header: {
            Text("Dados locais")
        } footer: {
            Text("Não há conta nem sincronização na cloud. A exportação cria um ficheiro JSON escolhido por si.")
        }
    }

    private var privacy: some View {
        Section {
            Label("Bluetooth apenas em primeiro plano", systemImage: "lock.iphone")
            Label("Sem conta e sem serviço remoto", systemImage: "person.crop.circle.badge.xmark")
            Label("Sem capturas brutas neste modo", systemImage: "waveform.badge.xmark")
        } header: {
            Text("Privacidade e limites")
        } footer: {
            Text("Ao sair da app ou perder a ligação, um treino ativo entra em pausa. A app não promete recolha com o ecrã bloqueado.")
        }
    }

    private var about: some View {
        Section {
            LabeledContent("Protocolo", value: "BLE v2")
            LabeledContent("Dados apresentados", value: "Eventos, cadência e FSR relativo")
        } header: {
            Text("Sobre")
        } footer: {
            Text("A leitura FSR é uma contagem do sensor. Não é uma medição calibrada de força, potência ou técnica.")
        }
    }
}
