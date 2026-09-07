# Estado da tarefa iPhone

Atualizado em 7 de setembro de 2026. A implementação SwiftUI iOS 17+ foi concluída nesta pasta pela retoma Terra Max. O projeto não altera firmware nem pede raw captures.

## Entrega presente

- `Sources/` contém transporte CoreBluetooth, descodificador v2, domínio, persistência local, origem de demonstração, estado de treino e interface SwiftUI em português.
- `SmartBoxingGlove.xcodeproj` inclui os destinos da app e `SmartBoxingGloveTests`, com o esquema partilhado `SmartBoxingGlove`.
- `Config/Info.plist` inclui a explicação portuguesa de `NSBluetoothAlwaysUsageDescription`.
- `Tests/Fixtures/protocol-v2-fixtures.json` é partilhado pelos XCTest e por `Tests/verify_protocol_fixtures.py`.
- `README.md` explica abertura no Xcode, limites do produto, protocolo, persistência e validação pendente.

## Comportamento implementado

O perfil BLE escreve exclusivamente `47 42 02 05` depois de as notificações estarem ativas; só passa a pronto depois de um `HELLO_ACK` v2 válido. Usa header de 6 bytes, ACK com payload de 6 bytes, EVENT com payload de 12 bytes e estatísticas de 23 contadores em 8 chunks. Os chunks 0–6 transportam três `uint32` (frame total de 20 bytes) e o chunk 7 transporta dois (frame total de 16 bytes). O descodificador valida ADC até 4095, sessão, sequência modular, capacidades, flags e comprimentos antes de produzir eventos.

A ligação tem escolha explícita de periférico, limites de tempo para procura/ligação/preparação/HELLO, guards para callbacks do periférico ativo e proteção contra callbacks de subscrição repetidos. Perder ligação, renovar a subscrição ou deixar o primeiro plano pausa uma sessão ativa. Não há recolha em segundo plano.

A persistência guarda datas com milissegundos, usa uma fila serial em segundo plano para checkpoints coalescidos e força gravação ordenada ao pausar, terminar ou alterar definições. Se os ficheiros principal e backup forem ilegíveis, a app entra em recuperação só de leitura e preserva os ficheiros até a pessoa escolher explicitamente apagá-los. Uma sessão encontrada como ativa após relançar é pausada no checkpoint guardado sem cobrar o período em que a app esteve fechada; durante a execução, a duração usa um relógio monotónico do processo. Demonstrações, histórico e progresso real ficam isolados.

## Validação efetuada neste ambiente

- A estrutura de `Info.plist` e do esquema Xcode foi validada como XML.
- O verificador estrutural Python partilhado passou: `protocol-v2 fixtures: OK`.
- Foram inspecionadas as referências de fontes, testes e recursos do projeto.

O ambiente Windows não tem Swift, Xcode nem `xcodebuild`. Não foi afirmada compilação Swift, execução XCTest, assinatura, simulador, ligação BLE ou teste num iPhone. O gate restante é abrir o projeto no Xcode no Mac, compilar para simulador, executar `SmartBoxingGloveTests`, instalar num iPhone e validar a luva ESP32-C3 real, incluindo autorização, Bluetooth desligado, timeouts, várias luvas, ACK inválido, reconexão, duplicados/wrap e ida para segundo plano.
