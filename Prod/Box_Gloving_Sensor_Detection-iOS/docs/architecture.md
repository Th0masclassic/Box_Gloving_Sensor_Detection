# Arquitetura da aplicação iPhone

Decisão: aplicação nativa SwiftUI, iOS 17 ou posterior, CoreBluetooth como central BLE e persistência local Codable. O MVP acompanha treinos, impactos detetados, cadência, histórico e objetivos. Não classifica técnica, não estima velocidade do punho e não apresenta força física calibrada. A arquitetura usa apenas eventos e estatísticas do firmware existente; não exige alteração do ESP32-C3.

## Separação de responsabilidades

- **Apresentação SwiftUI:** início, treino, histórico, progresso e ligação; texto em português, estados vazios úteis e indicação permanente quando a origem é demonstração.
- **Estado da aplicação:** coordena origem dos eventos, sessão de treino, pausa/finalização e gravação. Atualizações observáveis e callbacks BLE ficam serializados na main queue; não se partilha estado mutável entre callbacks concorrentes.
- **Transporte CoreBluetooth:** descoberta, ligação, subscrição, HELLO, validação de sessão e diagnóstico de ligação. A ligação BLE é distinta da sessão de treino.
- **Descodificador puro:** recebe bytes e produz ACK, evento ou estatística; valida tamanhos antes de ler. Não depende de SwiftUI nem de CoreBluetooth, permitindo testes de fixtures no Mac.
- **Origem de demonstração:** produz eventos sintéticos identificados. Não altera recordes, objetivos ou conquistas de treinos reais. Mudar de origem durante um treino exige terminar ou descartar esse treino.
- **Repositório local:** ficheiro JSON versionado em Application Support, escrita atómica e erros visíveis. Uma falha de gravação não deve apagar o treino em memória nem anunciar sucesso. Dados ilegíveis não são silenciosamente substituídos por um histórico vazio.

Modelos recomendados: evento com origem, identificador local, sessão BLE, sequência, punch ID, timestamp do dispositivo, instante de receção, ADC e flags; treino com UUID, início, duração ativa, origem e eventos/resumo; preferências e objetivos separados. Guardar apenas o necessário para histórico e evolução; nenhuma conta ou serviço remoto é necessário.

## Contrato BLE confirmado no código

Fontes: `../Box_Gloving_Sensor_Detection-Firmware/components/drivers/include/glove_protocol.h`, `components/drivers/src/glove_protocol.c`, `components/drivers/src/bluetooth_driver.c` e `main/main.c` dentro dessa pasta de firmware. Estes caminhos são relativos à raiz do projeto iOS, exceto os sufixos indicados.

| Elemento | Valor |
| --- | --- |
| Serviço | `12345678-1234-5678-1234-56789abcdef0` |
| Característica read/write/notify | `12345678-1234-5678-1234-56789abcdef1` |
| HELLO | `47 42 02 05` hexadecimal: GB, versão 2, eventos + estatísticas |
| Capturas | Não pedir capability `0x02`; não há montagem de raw captures neste MVP |
| Endianness | Todos os inteiros multibyte são little endian |

A máquina de estados passa por Bluetooth indisponível/desligado/sem autorização, inativo, procura, ligação, descoberta, subscrição, negociação e pronto. Só fazer scan com o central em `poweredOn`. Filtrar pelo UUID do serviço, permitir seleção explícita do periférico e conservar a referência ao periférico escolhido. Usar timeout para estados transitórios e ignorar callbacks de periféricos que já não são o ativo.

Depois de descobrir a característica, chamar `setNotifyValue(true, for:)`. Só escrever HELLO com `.withResponse` depois do callback `didUpdateNotificationStateFor` sem erro e com `isNotifying == true`: o firmware rejeita HELLO antes da subscrição. A confirmação da escrita não substitui o ACK do protocolo. Só apresentar pronto após ACK válido. Uma reconexão volta a descobrir/subscrever/negociar e limpa sequência, deduplicação e estado transitório de estatísticas.

### Notificações

Cada notificação contém um frame completo. Header de 6 bytes: offset 0 = `0xB2`, offset 1 = tipo, offsets 2–3 = sessão uint16, offsets 4–5 = sequência uint16. O byte de versão pertence ao ACK, não ao header.

| Tipo | Comprimento total | Payload após o header |
| --- | --- | --- |
| `0x01` HELLO_ACK | 12 | versão uint8; capabilities uint8; frequência accel uint16; frequência mag uint16 |
| `0x10` EVENT | 18 | punch ID uint16; trigger time µs uint32; FSR ADC uint16; valor legado force centi-kg uint16; flags uint16 |
| `0x13` STATS | 20 nos chunks 0–6, 16 no chunk 7 | índice uint8; total de chunks uint8 (=8); até três contadores uint32 |

EVENT flag bit 0 indica FSR válido; bits 1 e 2 indicam captura alocada/perdida. O evento fornece a amostra no momento de disparo, não o pico de uma captura completa. O valor legado centi-kg não demonstra calibração física; preservar internamente se necessário, mas não o apresentar como kg, N ou potência. Um gráfico ADC deve ser identificado como leitura/intensidade relativa do sensor e tratar valores inválidos como indisponíveis, nunca como zero medido.

STATS índices globais: 0 ciclos de aquisição, 1 timeouts de data-ready, 2 notificações data-ready coalescidas, 3–6 erros de leitura ACCEL/GYRO/MAG/FSR, 7 slots de captura indisponíveis ou falha de inicialização, 8 perdas da fila de eventos, 9 perdas da fila de capturas concluídas, 10 falhas de transmissão fora de sessão, 11 perdas de transmissão por mudança de sessão, 12 perdas de eventos por retry/deadline, 13 perdas de capturas por retry/deadline, 14 triggers do detetor de limiar, 15 gaps de timestamp de amostra acima de 1,5 períodos, 16 maior gap observado em microssegundos, 17 trabalhos de aquisição acima de um período, 18 maior duração de trabalho de aquisição em microssegundos e 19–22 contagens de amostras ACCEL/GYRO/MAG frescas e FSR válido. O firmware atual envia 23 contadores em 8 chunks: os índices de chunk 0–6 trazem três `uint32` e o chunk 7 traz dois, com cada chunk a começar em `chunkIndex * 3`. São contadores de execução do firmware, não totais de socos nem valores exclusivos do treino atual. Não há identificador de snapshot; não presumir que uma coleção parcial de chunks é uma fotografia atómica.

Tipos de captura `0x11/0x12` são inesperados neste perfil e não devem bloquear a receção futura. `0x7F` está reservado como ERROR, mas não há payload de erro definido no encoder atual: apresentar diagnóstico genérico, sem inventar estrutura. Rejeitar frames curtos, comprimentos incompatíveis, ACK de versão/capabilities incorretas e dados de outra sessão. Registar frames desconhecidos sem interpretar dados arbitrários como impactos.

### Sequência e tempo

A sequência é global a todos os tipos de frame. Um salto indica notificações em falta, **não** um número conhecido de impactos em falta. Usar aritmética modular de uint16 para wrap, eliminar duplicados e descartar frames antigos. Os punch IDs também podem dar wrap e avançam mesmo quando não há sessão ativa; não deduzir treino a partir do seu valor absoluto. Uma chave de evento deve incluir a geração local da ligação/sessão e não apenas punch ID.

O timestamp de firmware é uptime em microssegundos uint32 e dá wrap aproximadamente a cada 71,6 minutos. Não é uma data UTC. Usar relógio local para datas e duração de treino, com cálculo de duração ativa resistente a pausas; diferenças de firmware, se usadas, têm de ser modulares dentro de uma sessão. Nunca ligar a cronologia de duas conexões através do timestamp do dispositivo.

## Comportamento de treino e progresso

Só aceitar eventos enquanto existe treino ativo e não pausado. Ao perder ligação, pausar ou finalizar explicitamente, informar o utilizador e não preencher o intervalo perdido com dados sintéticos. Uma retoma necessita de ligação pronta. Sem suporte de background validado, interromper o treino ao sair da aplicação; não prometer recolha com ecrã bloqueado. O timer visual pode atualizar o ecrã, mas não deve ser a fonte de verdade da duração.

Derivar contagem de eventos válidos, duração ativa e cadência observada. Objetivos e conquistas são regras locais transparentes baseadas em treinos reais guardados, por exemplo sessões concluídas ou contagem acumulada. Um objetivo editado não reescreve os factos de um treino anterior. Evitar recompensas que incentivem maximizar ADC ou força. Não há bateria, mão esquerda/direita, classificação jab/hook, calorias ou pontuação técnica disponíveis neste protocolo.

## Integração Apple e validação

Incluir `NSBluetoothAlwaysUsageDescription` com uma explicação em português. A Apple exige esta chave para acesso CoreBluetooth em aplicações ligadas aos SDKs de iOS 13 ou posterior: [Core Bluetooth](https://developer.apple.com/documentation/corebluetooth). A conclusão da subscrição é assíncrona e chega ao delegate: [setNotifyValue(_:for:)](https://developer.apple.com/documentation/corebluetooth/cbperipheral/setnotifyvalue(_:for:)). Referências verificadas em 6 de setembro de 2026.

No Windows podem rever-se os bytes, JSON/plist e referências do projeto; isso não valida compilação Swift, assinatura, simulador ou hardware. O gate no Mac é abrir o projeto no Xcode, compilar para simulador, executar testes de decoder e persistência, e instalar num iPhone com o ESP32-C3 real. Testar autorização negada, Bluetooth desligado, timeout, subscrição antes de HELLO, negociação inválida, desconexão a meio do treino, notificações duplicadas, wrap de sequência/timestamp, estatísticas incompletas e isolamento entre demo e progresso real. Não apresentar esta entrega como hardware-validada antes desses testes.
