# Smart Boxing Glove para iPhone

Aplicação nativa SwiftUI para iPhone (iOS 17+) que acompanha sessões com a Smart Boxing Glove através de Bluetooth Low Energy. A interface está em português e guarda tudo localmente no iPhone: não usa contas, cloud nem serviços remotos.

## O que a app faz

- Procura luvas BLE e deixa a pessoa escolher qual ligar; nunca escolhe automaticamente a primeira encontrada.
- Só fica pronta após ativar notificações e confirmar o `HELLO_ACK` do protocolo v2.
- Inicia, pausa, retoma e termina treinos, com contagem de eventos, duração ativa e cadência observada entre eventos consecutivos da parte ativa atual.
- Guarda histórico local, meta diária, conquistas baseadas em sessões reais concluídas e exportação JSON.
- Tem um modo de demonstração com sinais sintéticos, rotulado no treino e no histórico. Demonstrações não mudam metas, conquistas, totais ou progresso real.
- Pausa automaticamente um treino ao perder a ligação Bluetooth ou quando a app deixa o primeiro plano. Não recolhe dados em segundo plano.

O ecrã apresenta eventos da luva e a contagem FSR relativa (`0…4095`). Não declara força física, potência, velocidade, técnica de golpe, calorias ou análise por IA.

## Abrir no Xcode

1. Num Mac com Xcode 16 ou posterior, abra `SmartBoxingGlove.xcodeproj`.
2. Selecione o esquema partilhado **SmartBoxingGlove** e um simulador iOS 17+ para compilar e executar os testes.
3. Para testar Bluetooth, selecione um iPhone físico, configure a assinatura com a sua equipa Apple e instale a app.
4. Ligue a luva no separador **Treino**, escolha o periférico encontrado e aguarde o estado **Luva pronta** antes de iniciar um treino real.

O `Info.plist` já inclui `NSBluetoothAlwaysUsageDescription` em português. A execução Bluetooth exige um iPhone físico; o simulador não valida CoreBluetooth nem a luva.

## Protocolo BLE usado

O contrato autoritativo está em `../Box_Gloving_Sensor_Detection-Firmware/docs/PROTOCOL_V2.md`.

| Item | Valor |
| --- | --- |
| Serviço | `12345678-1234-5678-1234-56789abcdef0` |
| Característica | `12345678-1234-5678-1234-56789abcdef1` |
| HELLO | `47 42 02 05` — versão 2, eventos e estatísticas |
| Cabeçalho | 6 bytes, magic `B2`, tipo, sessão uint16 LE, sequência uint16 LE |
| EVENT | 18 bytes no total, payload de 12 bytes |
| HELLO_ACK | 12 bytes no total, payload de 6 bytes |
| STATS atual | 23 contadores em 8 chunks: índices 0–6 com 3 `uint32`; índice 7 com 2 `uint32` |

O cliente não pede a capability de capturas (`0x02`) e não monta dados brutos. Valida o ADC FSR de 12 bits, versões/capabilities, sessão, sequência modular, ACK pendente e tipos de frame antes de transformar qualquer notificação num evento de treino.

## Persistência e recuperação

O histórico é um JSON versionado em `Application Support/SmartBoxingGlove`, escrito de forma atómica com uma cópia segura do último estado válido. Datas novas usam milissegundos para preservar cadência histórica.

As chegadas de eventos não codificam e escrevem todo o histórico na fila principal: checkpoints são coalescidos numa fila serial em segundo plano. Pausar, terminar, alterar definições e recuperar uma sessão pedem uma gravação ordenada imediata. Uma falha de escrita mantém o treino em memória e apresenta um aviso; não anuncia sucesso.

Se o ficheiro principal e a cópia segura não puderem ser lidos, a app entra em modo de recuperação só de leitura. Os ficheiros não são substituídos por um histórico vazio nem apagados automaticamente. Nas **Definições**, a ação explícita de apagar dados remove os ficheiros e inicia um novo histórico vazio.

Uma sessão que estava marcada como em execução ao reabrir a app é colocada em pausa no último checkpoint guardado. O tempo em que a app esteve fechada não é adicionado à duração ativa. Durante uma execução, o contador usa o relógio monotónico do processo em vez do relógio de parede.

## Testes

O destino `SmartBoxingGloveTests` inclui testes XCTest para:

- HELLO, ACK, EVENT, ADC máximo, frames inválidos e sequência com wrap/duplicados;
- os oito chunks e os 23 contadores de estatísticas;
- persistência de milissegundos, recuperação pela cópia segura, preservação dos ficheiros ilegíveis e recuperação de sessão sem cobrar downtime.

O ficheiro partilhado [`Tests/Fixtures/protocol-v2-fixtures.json`](Tests/Fixtures/protocol-v2-fixtures.json) é consumido pelos XCTest e pelo verificador estrutural Python [`Tests/verify_protocol_fixtures.py`](Tests/verify_protocol_fixtures.py). No Mac ou em qualquer ambiente com Python 3:

```sh
python3 Tests/verify_protocol_fixtures.py
```

No Xcode, use **Product > Test** com o esquema partilhado. Para uma validação completa antes de distribuição, compile para simulador, execute os testes, instale num iPhone e verifique autorização negada, Bluetooth desligado, descoberta com várias luvas, timeout, ACK inválido, duplicados, wrap de sequência, ligação perdida e ida para segundo plano.

## Estado de validação nesta entrega

Os ficheiros do projeto, plist, esquema e fixtures foram revistos estruturalmente no ambiente Windows. O verificador Python partilhado foi executado com sucesso (`protocol-v2 fixtures: OK`). Este ambiente não inclui Swift, Xcode ou `xcodebuild`, por isso esta entrega não afirma compilação iOS, assinatura, testes XCTest, simulador ou hardware BLE validados. Esses passos continuam a exigir o Mac e o iPhone descritos acima.
