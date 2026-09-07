import SwiftUI

struct TrainingDashboardView: View {
    @EnvironmentObject private var model: TrainingViewModel
    @State private var confirmFinish = false

    private var activeSource: TrainingSource? { model.activeSession?.source }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    hero
                    if let safetyNotice = model.safetyNotice {
                        NoticeBanner(message: safetyNotice, tint: .orange) {
                            model.dismissSafetyNotice()
                        }
                    }
                    if !model.canWriteLocalData {
                        recoveryGate
                    }
                    connectionCard
                    sourceCard
                    trainingCard
                    transparencyNote
                }
                .padding(16)
                .padding(.bottom, 18)
            }
            .background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("Treino")
            .alert("Terminar treino?", isPresented: $confirmFinish) {
                Button("Continuar", role: .cancel) {}
                Button("Terminar", role: .destructive) {
                    model.endTraining()
                }
            } message: {
                Text("A sessão será guardada apenas neste iPhone. Os treinos de demonstração permanecem separados do progresso real.")
            }
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text("SMART BOXING GLOVE")
                .font(.caption.weight(.bold))
                .tracking(1.4)
                .foregroundStyle(AppPalette.accent)
            Text(activeSource == .demo ? "Treino de demonstração" : "Treino com a luva")
                .font(.system(.largeTitle, design: .rounded, weight: .bold))
            Text(activeSource == .demo
                 ? "Explore o ecrã com sinais sintéticos, claramente separados do seu progresso real."
                 : "Registe eventos enviados pela luva ligada e acompanhe a cadência observada.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .padding(.top, 6)
    }

    private var recoveryGate: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 10) {
                Label("Dados locais em recuperação", systemImage: "lock.fill")
                    .font(.headline)
                    .foregroundStyle(.orange)
                Text("Para preservar os ficheiros que a app não conseguiu ler, iniciar e alterar treinos está desativado. Pode apagar os dados de forma explícita nas Definições para começar de novo.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var connectionCard: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Ligação à luva")
                            .font(.headline)
                        Text(model.bluetooth.connectionState.title)
                            .font(.subheadline)
                            .foregroundStyle(model.bluetooth.isReady ? .green : .secondary)
                    }
                    Spacer()
                    if model.bluetooth.connectionState.isWorking {
                        ProgressView()
                            .accessibilityLabel("Ligação em curso")
                    } else {
                        Image(systemName: model.bluetooth.isReady ? "checkmark.circle.fill" : "antenna.radiowaves.left.and.right")
                            .font(.title2)
                            .foregroundStyle(model.bluetooth.isReady ? .green : AppPalette.accent)
                            .accessibilityHidden(true)
                    }
                }

                if model.bluetooth.isReady {
                    Button(role: .destructive) {
                        model.disconnectGlove()
                    } label: {
                        Label("Desligar luva", systemImage: "bolt.slash.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                } else if model.bluetooth.connectionState.isWorking {
                    Button(role: .cancel) {
                        model.disconnectGlove()
                    } label: {
                        Label("Cancelar ligação", systemImage: "xmark")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                } else {
                    Button {
                        model.connectGlove()
                    } label: {
                        Label("Procurar luvas", systemImage: "magnifyingglass")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(AppPalette.accent)
                }

                if !model.bluetooth.discoveredDevices.isEmpty {
                    Divider()
                    Text("Luva encontrada")
                        .font(.subheadline.weight(.semibold))
                    ForEach(model.bluetooth.discoveredDevices) { device in
                        Button {
                            model.connect(to: device)
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: "sensor.tag.radiowaves.forward")
                                    .foregroundStyle(AppPalette.accent)
                                    .accessibilityHidden(true)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(device.name)
                                        .foregroundStyle(.primary)
                                    Text("Sinal Bluetooth: \(device.rssi) dBm")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: "chevron.right")
                                    .font(.caption.weight(.bold))
                                    .foregroundStyle(.tertiary)
                            }
                            .padding(.vertical, 4)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Ligar a \(device.name)")
                    }
                    Text("Escolha a luva que pretende ligar. A app não seleciona automaticamente a primeira encontrada.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                if let issue = model.bluetooth.lastProtocolIssue,
                   !model.bluetooth.isReady {
                    Text(issue)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private var sourceCard: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 12) {
                SectionTitle("Origem do treino", subtitle: "A demonstração nunca altera metas, conquistas ou totais reais.")
                Picker("Origem", selection: $model.selectedSource) {
                    ForEach(TrainingSource.allCases) { source in
                        Text(source.title).tag(source)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(model.activeSession != nil || !model.canWriteLocalData)

                if model.selectedSource == .demo || activeSource == .demo {
                    Label("Modo demonstração — sinais sintéticos; não contam para o progresso real.", systemImage: "theatermasks.fill")
                        .font(.footnote.weight(.medium))
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var trainingCard: some View {
        SurfaceCard {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    SectionTitle(
                        model.activeSession == nil ? "Pronto para começar" : "Sessão em curso",
                        subtitle: sessionSubtitle
                    )
                    Spacer()
                    if model.isTrainingRunning {
                        Text("A REGISTAR")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 9)
                            .padding(.vertical, 5)
                            .background(.green, in: Capsule())
                    }
                }

                HStack(spacing: 10) {
                    MetricTile(
                        title: "Duração ativa",
                        value: TimeInterval(model.elapsedDisplay).clockText,
                        detail: "Não conta pausas",
                        symbol: "timer"
                    )
                    MetricTile(
                        title: "Eventos",
                        value: "\(model.activePunchCount)",
                        detail: "Nesta sessão",
                        symbol: "circlebadge.fill"
                    )
                    MetricTile(
                        title: "Cadência",
                        value: model.currentCadence.map { "\($0)" } ?? "—",
                        detail: "golpes/min observados",
                        symbol: "waveform.path.ecg",
                        tint: .orange
                    )
                }

                if let lastPunch = model.lastPunch {
                    lastPunchFeedback(lastPunch)
                } else {
                    Text(model.isTrainingRunning
                         ? "À espera do próximo evento válido."
                         : "A cadência é calculada apenas entre eventos da parte ativa atual.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                controls
            }
        }
    }

    private var sessionSubtitle: String {
        guard let session = model.activeSession else {
            return model.selectedSource == .demo ? "Experimentar sem afetar progresso" : "Requer uma luva pronta"
        }
        if session.source == .demo {
            return "Demonstração separada"
        }
        return session.state == .running ? "Eventos da luva em primeiro plano" : "Em pausa"
    }

    @ViewBuilder
    private func lastPunchFeedback(_ punch: PunchRecord) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "hand.raised.fill")
                .foregroundStyle(AppPalette.accent)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("Evento registado")
                    .font(.subheadline.weight(.semibold))
                if let adc = punch.fsrADC {
                    Text("Leitura relativa do sensor FSR: \(adc) / 4095")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Text("Sinal de demonstração")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.green)
                .accessibilityHidden(true)
        }
        .padding(12)
        .background(Color.green.opacity(0.10), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private var controls: some View {
        if model.activeSession == nil || model.activeSession?.state == .paused {
            Button {
                model.startOrResumeTraining()
            } label: {
                Label(model.activeSession == nil ? "Iniciar treino" : "Retomar treino", systemImage: "play.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(AppPalette.accent)
            .disabled(!model.canWriteLocalData)
        } else {
            HStack(spacing: 12) {
                Button {
                    model.pauseTraining()
                } label: {
                    Label("Pausar", systemImage: "pause.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)

                Button(role: .destructive) {
                    confirmFinish = true
                } label: {
                    Label("Terminar", systemImage: "stop.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }
        }

        if model.activeSession?.state == .paused {
            Button(role: .destructive) {
                confirmFinish = true
            } label: {
                Label("Guardar e terminar", systemImage: "stop.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .frame(maxWidth: .infinity)
        }
    }

    private var transparencyNote: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "info.circle")
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            Text("A app apresenta eventos e leituras relativas do sensor recebidos da luva. Não estima força física, técnica de golpe, velocidade, potência nem faz análise por IA.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 4)
    }
}
