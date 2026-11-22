<p align="center">
  <img src="logo_2.png" alt="Logo UTFPR - Universidade Tecnológica Federal do Paraná" width="340">
</p>

<h3 align="center">Universidade Tecnológica Federal do Paraná (UTFPR)</h3>
<p align="center">
  <strong>Disciplina:</strong> OPT101 — Tópicos Especiais Em Interfaces Computacionais
</p>

<h1 align="center">Rede IoT Sem Fios (2,4 GHz) com Controle Autônomo M2M, CSMA/CA, ARQ e Supervisão Web Multi-Porta</h1>

---

## Sumário

1. [Visão Geral do Projeto](#1-visão-geral-do-projeto)
2. [Arquitetura da Rede e Papéis dos Nós (`MEU_ID`)](#2-arquitetura-da-rede-e-papéis-dos-nós-meu_id)
3. [Hardware, Pinagem SPI e Estabilidade Elétrica](#3-hardware-pinagem-spi-e-estabilidade-elétrica)
4. [Anatomia do Quadro e Endereçamento (`struct PacoteIoT`)](#4-anatomia-do-quadro-e-endereçamento-struct-pacoteiot)
5. [Protocolos de Acesso ao Meio e Confiabilidade (`CSMA/CA` + `ARQ`)](#5-protocolos-de-acesso-ao-meio-e-confiabilidade-csmaca--arq)
6. [Gestão de Espectro, Interferência Wi-Fi e *Auto-Scan*](#6-gestão-de-espectro-interferência-wi-fi-e-auto-scan)
7. [Diagramas Temporais e Máquinas de Estado](#7-diagramas-temporais-e-máquinas-de-estado)
8. [Guia de Operação da Interface Web (`script.py`)](#8-guia-de-operação-da-interface-web-scriptpy)

---

## 1. Visão Geral do Projeto

Este projeto implementa uma **rede de sensores e atuadores sem fios na faixa ISM de $2{,}4\text{ GHz}$** utilizando microcontroladores **Arduino Nano (ATmega328P)** e transceptores **nRF24L01+**, integrada a um painel de monitoramento e diagnóstico web multi-porta em Python (`script.py`).

A solução combina **controle autônomo máquina-a-máquina (M2M)** — permitindo que o nó sensor acione o nó atuador diretamente sem depender de um computador central — com **supervisão em estrela via Gateway**, possibilitando telemetria em tempo real, comando remoto, ajuste dinâmico de limiares e avaliação experimental de interferência em canais compartilhados com redes Wi-Fi.

### Principais Características Técnicas

* **Arquitetura Parametrizada por Papel (`MEU_ID`):** O firmware organiza as três placas da rede (`Gateway`, `Sensor` e `Atuador`) sob a mesma estrutura de quadro, temporizações e regras de enlace.
* **Prevenção de Colisões (`CSMA/CA`):** Escuta física de portadora antes da transmissão via registrador `RPD` (*Received Power Detector*, limiar de $-64\text{ dBm}$) combinada com atraso aleatório (*random backoff*).
* **Confiabilidade Ponta a Ponta (`ARQ` + `CRC-16`):** Verificação de integridade de 16 bits em hardware e protocolo *Stop-and-Wait* em software com confirmações explícitas (`MSG_ACK`), deduplicação por número de sequência (`seq`) e *backoff* exponencial binário.
* **Resiliência de Espectro e Hardware:** Sincronização de canal por difusão (*broadcast* de `MSG_BEACON`), varredura autônoma de canais (`Auto-Scan` de `0` a `125`) e auto-recuperação do transceptor (`garantirRadioAtivo()`).

---

## 2. Arquitetura da Rede e Papéis dos Nós (`MEU_ID`)

<p align="center">
  <img src="Gemini_Generated_Image_k7i29rk7i29rk7i2(1).jpg" alt="Arquitetura Híbrida de Três Nós com Arduino Nano e nRF24L01" width="100%">
</p>

A topologia opera em **dois saltos no sentido ascendente** (telemetria consolidada) e **difusão direta no sentido descendente** (controle e sincronização de parâmetros):

```text
+-------------------+    1. MSG_DADOS_M2M (A0)     +-------------------+
|   NÓ 1 (Sensor)   | ---------------------------> |  NÓ 2 (Atuador)   |
|     MEU_ID = 1    | <--------------------------- |     MEU_ID = 2    |
|   Pino A0 (ADC)   |         2. MSG_ACK           |   Pino D4 (Saída) |
+-------------------+                              +-------------------+
         ^                                                |     ^
         |                               3. MSG_TELEMETRIA|     | 4. MSG_ACK
         |                                                v     |
         |         6. MSG_BEACON (Broadcast: 255)  +-------------------+
         +---------------------------------------- |  GATEWAY (Nó 0)   |
                                                   |     MEU_ID = 0    |
                                                   +-------------------+
                                                          |     ^
                                            5. JSON (TX)  |     | 7. Comandos (RX)
                                            (115200 bps)  v     | (LED / CH / LIM)
                                                   +-------------------+
                                                   |  PC (script.py)   |
                                                   |  Interface Web    |
                                                   +-------------------+
```

### Divisão Funcional por Identificador (`MEU_ID`)

| Identificador | Papel na Rede | Responsabilidades Principais |
| :--- | :--- | :--- |
| **`MEU_ID 0`** | **Gateway** | Faz a ponte entre o enlace RF e a porta USB ($115200\text{ bps}$). Recebe `MSG_TELEMETRIA` do Nó 2, serializa o estado em JSON para o backend Python e transmite `MSG_BEACON` (`destino = 255`) para sincronizar canal, modo (`AUTO`/`MANUAL`) e limiar. |
| **`MEU_ID 1`** | **Nó Sensor** | Realiza a conversão analógico-digital de 10 bits no pino `A0` (`0..1023`) a cada $1200\text{ ms}$ e envia o quadro `MSG_DADOS_M2M` diretamente ao Nó 2 (`destino = 2`). |
| **`MEU_ID 2`** | **Nó Atuador / Concentrador** | Recebe os dados do Nó 1, confirma com `MSG_ACK`, decide localmente o acionamento do pino `D4` (quando em modo `AUTO`) e encaminha o estado consolidado junto às métricas de enlace (`RPD` e `Retries`) ao Gateway via `MSG_TELEMETRIA`. |

---

## 3. Hardware, Pinagem SPI e Estabilidade Elétrica

### Mapa de Conexões (Arduino Nano $\leftrightarrow$ nRF24L01+)

| Pino nRF24L01+ | Pino Arduino Nano | Função no Barramento |
| :--- | :--- | :--- |
| `VCC` | `3.3V` | Alimentação do transceptor (recomenda-se capacitor de $10\ \mu\text{F}$ entre `3.3V` e `GND`) |
| `GND` | `GND` | Referência comum |
| `CE` | `D7` | *Chip Enable* — seleciona modos RX/TX do transceptor |
| `CSN` | `D8` | *Chip Select Not* — habilita comunicação no barramento SPI |
| `SCK` | `D13` | *Serial Clock* do SPI (**não utilizar `D13` como saída de atuador**) |
| `MOSI` | `D11` | *Master Out Slave In* (dados Arduino $\rightarrow$ nRF24) |
| `MISO` | `D12` | *Master In Slave Out* (dados nRF24 $\rightarrow$ Arduino) |
| **Sensor (Nó 1)** | `A0` | Entrada analógica do sensor (`0..1023`) |
| **Atuador (Nó 2)** | `D4` | Saída digital de acionamento (utilizar resistor de $220\ \Omega$ a $1\text{ k}\Omega$ em série com LED) |

### Decisões de Engenharia Elétrica

1. **Calibração Diferenciada de Potência (`setPALevel`):**
   * **Gateway (`MEU_ID 0`):** Configurado em `RF24_PA_MIN` ($-18\text{ dBm}$). Por utilizar módulo com amplificador externo (`PA+LNA`), níveis maiores drenam picos de corrente acima da capacidade do regulador de $3{,}3\text{ V}$ do Arduino Nano e saturam os receptores vizinhos na bancada.
   * **Nós Remotos (`MEU_ID 1` e `2`):** Configurados em `RF24_PA_LOW` ($-12\text{ dBm}$), oferecendo margem de potência $4\times$ superior para atravessar interferências Wi-Fi sem comprometer a estabilidade da alimentação.
2. **Proteção de Corrente no Atuador (`D4`):** Ligar um LED diretamente entre `D4` e `GND` sem resistor limitador drena corrente excessiva do microcontrolador, provocando queda de tensão na linha de $3{,}3\text{ V}$ e travando o rádio do Nó 2. O resistor limitador mantém a tensão estável durante o acionamento.
3. **Auto-Recuperação de Hardware (`garantirRadioAtivo()`):** A cada ciclo de execução, o firmware valida a conexão SPI e o canal ativo no chip; caso uma oscilação elétrica reinicie os registradores do nRF24L01+, o módulo é reinicializado e reconfigurado em menos de $1\text{ ms}$.

---

## 4. Anatomia do Quadro e Endereçamento (`struct PacoteIoT`)

Cada transmissão combina o cabeçalho físico gerenciado pelo chip nRF24L01+ (operando a $250\text{ kbps}$ para máxima sensibilidade de recepção, $\approx -94\text{ dBm}$) com a estrutura binária de **15 bytes** definida na camada de protocolo:

```text
CAMADA FÍSICA (Controlada pelo Chip nRF24L01+ em 250 kbps)
+-----------+----------------+----------+------------------------+----------+
| Preâmbulo | Endereço Rede  | Controle |   Payload (15 Bytes)   |  CRC-16  |
|  (1 Byte) |   "IOT26" (5B) | (9 bits) |   struct PacoteIoT     | (2 Bytes)|
+-----------+----------------+----------+------------------------+----------+
                     |                                                |
                     v                                                v
          Filtra outras equipes                            Descarta quadro se
          direto no hardware                               ruído alterar 1 bit

DETALHAMENTO DO PAYLOAD — struct PacoteIoT (15 Bytes)
+------+--------+---------+-----+-------+--------+------+--------+--------+---------+---------+
| tipo | origem | destino | seq | canal | atuad. | auto | sensor | limiar | rpdHits | retries |
|  1B  |   1B   |   1B    | 1B  |  1B   |   1B   |  1B  |   2B   |   2B   |   2B   |   2B    |
+------+--------+---------+-----+-------+--------+------+--------+--------+---------+---------+
  1=BCN  0, 1, 2  0, 1, 2  1..255 0..125   0 / 1   0 / 1  0..1023  padrão    ruído    retransm.
  2=M2M           255=ALL                                           500      CSMA       ARQ
  3=TEL
  4=ACK
```

### Mecanismos de Controle do Quadro

* **Isolamento Lógico por Endereço (`"IOT26"`):** Ambos os *pipes* de transmissão e recepção utilizam a assinatura de 5 bytes `"IOT26"`. O hardware descarta automaticamente quadros de outras equipes operando na mesma frequência.
* **Confirmações Endereçadas (`setAutoAck(false)` + `MSG_ACK`):** Como todos os nós escutam o mesmo endereço para receber *Beacons* em difusão (`destino = 255`), o *Auto-ACK* nativo foi desativado para evitar que os Nós 1 e 2 respondam simultaneamente ao Gateway. A confirmação é feita por um quadro `MSG_ACK` explícito contendo o `seq` original.
* **Deduplicação com Expiração (`pacoteNovo()`):** Se um `MSG_ACK` for perdido por ruído no retorno, o transmissor reenviará o quadro com o mesmo `seq`. O receptor compara `seq != ultimoSeqRecebido` (com janela de expiração de $3000\text{ ms}$) para impedir contagem duplicada no Gateway.

---

## 5. Protocolos de Acesso ao Meio e Confiabilidade (`CSMA/CA` + `ARQ`)

<p align="center">
  <img src="Gemini_Generated_Image_k7i29rk7i29rk7i2(2).jpg" alt="Fluxograma do Protocolo CSMA/CA, CRC-16 e ARQ Stop-and-Wait" width="100%">
</p>

### Escolha Técnica: CSMA/CA + ARQ em vez de MACA (`RTS/CTS`)

O sistema implementa **CSMA/CA não-persistente com detecção física de portadora (`RPD`)** aliado ao **ARQ *Stop-and-Wait* com *backoff* exponencial**, em vez do protocolo **MACA** (*Multiple Access with Collision Avoidance* baseado na troca prévia `RTS/CTS`):

1. **Eficiência para Quadros Curtos:** A carga útil possui apenas **15 bytes**. Preceder cada leitura de sensor com quadros `RTS` e `CTS` triplicaria a ocupação do canal para transportar 2 bytes úteis de leitura analógica.
2. **Latência Half-Duplex do nRF24L01+:** Cada comutação RX $\leftrightarrow$ TX exige $130\ \mu\text{s}$ de estabilização do oscilador PLL; uma troca em 4 etapas (`RTS-CTS-DATA-ACK`) multiplicaria o tempo morto e os picos de corrente.
3. **Convivência com Redes Wi-Fi:** Roteadores Wi-Fi não interpretam quadros `RTS/CTS` do nRF24L01+. A escuta física de energia no espectro via `radio.testRPD()` detecta diretamente a presença de portadoras Wi-Fi acima de $-64\text{ dBm}$.

### Fluxograma Interno do Envio Confiável (`enviarConfiavel()`)

```text
                         [ Início: enviarConfiavel(pkt) ]
                                        |
                                        v
                        cw = 20 ms | tentativa = 0 (máx 5)
                                        |
              +------------------------>+
              |                         v
              |             +-----------------------+
              |             |     canalLivre()      |  <-- CSMA/CA: 6 leituras
              |             |   (testRPD > -64dBm)  |      espaçadas de 40 us
              |             +-----------------------+
              |                |                 |
              |            [Ocupado]          [Livre]
              |                |                 |
              |                v                 v
              |         totalRpdHits++     enviarRaw(pkt)
              |         delay(rand(5,cw))   (TX por 130 us)
              |         (até 6 esperas)          |
              |                |                 v
              |                +--------> +---------------+
              |                           | aguardarAck() | <-- Escuta por 100 ms
              |                           | (até 100 ms)  |     (se chegar Beacon,
              |                           +---------------+      atualiza canal/modo)
              |                              |         |
              |                         [Timeout]  [MSG_ACK OK]
              |                              |         |
              |                              v         v
              |                     totalRetries++   [ Retorna TRUE ]
              |                     delay(rand(15,cw))
              |                     cw = min(cw*2, 250)  <-- Backoff Exponencial
              |                     tentativa++
              |                              |
              +---- (se tentativa < 5) ------+
                                             |
                                      (se tentativa == 5)
                                             v
                                     [ Retorna FALSE ]
                                     (Exibe [ERR] no log)
```

---

## 6. Gestão de Espectro, Interferência Wi-Fi e *Auto-Scan*

<p align="center">
  <img src="Gemini_Generated_Image_k7i29rk7i29rk7i2.jpg" alt="Mapa de Espectro 2.4 GHz, Sobreposição Wi-Fi e Auto-Scan" width="100%">
</p>

A frequência central de operação do transceptor nRF24L01+ nos 126 canais disponíveis (`0` a `125`) segue a relação:

$$
f(\text{MHz}) = 2400 + \text{Canal}
$$

| Predefinição na Interface | Canal nRF24 | Frequência Central | Característica Espectral |
| :--- | :---: | :---: | :--- |
| **WiFi 1 (12)** | `12` | $2412\text{ MHz}$ | Centro do **Canal 1 do Wi-Fi** ($2401\text{–}2423\text{ MHz}$) — validação sob interferência real |
| **WiFi 6 (37)** | `37` | $2437\text{ MHz}$ | Centro do **Canal 6 do Wi-Fi** ($2426\text{–}2448\text{ MHz}$) — validação sob interferência real |
| **WiFi 11 (62)** | `62` | $2462\text{ MHz}$ | Centro do **Canal 11 do Wi-Fi** ($2451\text{–}2473\text{ MHz}$) — validação sob interferência real |
| **Limpo (100)** | `100` | $2500\text{ MHz}$ | Acima do limite superior do Wi-Fi ($\approx 2484\text{ MHz}$) — operação livre de interferência |

### Sincronização de Frequência

* **Migração Coordenada (`CH:`):** Ao alterar o canal pela interface web, o Gateway dispara **10 rajadas seguidas** de `MSG_BEACON` espaçadas por $18\text{ ms}$ na frequência antiga informando o canal de destino, migra seu próprio rádio e mantém *Beacons* acelerados a cada $80\text{ ms}$ durante $5\text{ s}$.
* **Recuperação Autônoma (`buscarCanalGateway()`):** Caso um nó remoto reinicie ou perca a rajada de troca de canal, após $5000\text{ ms}$ sem tráfego ele varre sequencialmente todos os canais (`0` a `125`, escutando $25\text{ ms}$ por canal) até captar qualquer quadro válido da rede e travar novamente na frequência ativa.

---

## 7. Diagramas Temporais e Máquinas de Estado

### Sequência Temporal de um Ciclo Completo

Para evitar colisões entre o primeiro salto (`Nó 1 -> Nó 2`) e o segundo salto (`Nó 2 -> Gateway`), o Nó 2 insere uma janela de guarda de $15\text{ ms}$ após devolver o `MSG_ACK` ao Nó 1:

```text
NÓ 1 (Sensor)              NÓ 2 (Atuador)              GATEWAY (ID 0)            PC (Web UI)
     |                           |                           |                        |
     |--[CSMA/CA: RPD Livre]     |                           |                        |
     |-- MSG_DADOS_M2M (seq=5) ->|                           |                        |
     |                           |-- (pausa 2 ms: RX->TX)    |                        |
     |<---- MSG_ACK (seq=5) -----|                           |                        |
     |                           |-- [Decide D4 se AUTO]     |                        |
     |                           |-- (Janela de guarda 15ms) |                        |
     |                           |--[CSMA/CA: RPD Livre]     |                        |
     |                           |-- MSG_TELEMETRIA (seq=9)->|                        |
     |                           |                           |-- (pausa 2 ms: RX->TX) |
     |                           |<---- MSG_ACK (seq=9) -----|                        |
     |                           |                           |-- JSON Serial -------->|
     |                           |                           |                        |
     |<================ MSG_BEACON (a cada 1500 ms) =========|                        |
```

### Máquina de Estados de Resiliência dos Nós Remotos

```text
                      +-----------------------------------+
                      |         OPERAÇÃO NORMAL           |
                      |  - Verifica garantirRadioAtivo()  |
                      |  - Escuta Beacons e dados M2M     |
                      +-----------------------------------+
                         |              |               ^
    Recebe MSG_BEACON    |              | > 5000 ms     | Encontra tráfego
    com novo canal       |              | sem ouvir     | ou reinicializa chip
                         v              v               |
            +-----------------+   +-------------------------+
            |  TROCA DIRETA   |   |   AUTO-SCAN (Varredura) |
            | aplicarCanal()  |   | buscarCanalGateway()    |
            | (Imediato)      |   | Varre CH 0..125 (25ms)  |
            +-----------------+   +-------------------------+
                     |                          |
                     +------------+-------------+
                                  |
                                  v
                     [ Sincronizado no Canal Ativo ]
```

---

## 8. Guia de Operação da Interface Web (`script.py`)

1. **Inicialização do Monitor Multi-Porta:** Feche o *Serial Monitor* da Arduino IDE e execute `python3 script.py` para abrir o servidor web local e escutar simultaneamente todas as portas `/dev/ttyUSB*` conectadas ao computador.
2. **Sincronização Inicial via DTR:** Utilize o botão **Resetar Placas (DTR)** na interface para enviar um pulso sincronizado de reinicialização aos três microcontroladores e confirmar na tabela de portas a identificação de `GATEWAY (ID 0)`, `NÓ 1 (SENSOR)` e `NÓ 2 (ATUADOR)`.
3. **Comandos de Supervisão e Controle:**
   * `LED:1` / `LED:0` — Alterna para o modo `MANUAL` e aciona ou desliga o pino `D4` no Nó 2.
   * `AUTO:1` — Restaura a autonomia M2M (`Sensor < Limiar` $\rightarrow$ `Atuador = 1`).
   * `LIM:<valor>` — Atualiza dinamicamente o limiar de comparação do sensor (ex.: `LIM:300` ou `LIM:600`).
   * `CH:<0..125>` — Migra toda a rede para um canal específico (`100` para operação limpa; `12`, `37` ou `62` para demonstração prática de `CSMA/CA` e `ARQ` sob tráfego Wi-Fi).