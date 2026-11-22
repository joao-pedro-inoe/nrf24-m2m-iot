# Rede IoT Multi-Salto com nRF24L01 (CSMA/CA + ARQ)

Sistema de telemetria e controle M2M (Machine-to-Machine) resiliente a interferências na faixa de 2.4 GHz, utilizando Arduino Nano, módulos transceptores nRF24L01 e um gateway de monitoramento web em Python.

## Arquitetura da Rede

- **Nó 1 (`MEU_ID = 1`) — Sensor:** Lê o pino analógico `A0` e transmite pacotes M2M diretamente para o Nó 2.
- **Nó 2 (`MEU_ID = 2`) — Atuador:** Recebe a leitura do Nó 1, aciona a saída digital `D4` (modo automático por limiar ou manual via Beacon) e encaminha a telemetria consolidada ao Gateway.
- **Nó 0 (`MEU_ID = 0`) — Gateway:** Sincroniza a rede via mensagens de *Beacon*, permite troca dinâmica de canal RF (0–125) e envia os dados em JSON via Serial para o servidor Python.

### Mecanismos de Confiabilidade
- **CSMA/CA:** Escuta de portadora via `testRPD()` (limiar de -64 dBm) antes de transmitir.
- **ARQ Stop-and-Wait:** Confirmação explícita (`MSG_ACK`) com *backoff* exponencial e deduplicação por número de sequência.
- **Agilidade de Canal & Auto-Scan:** Troca remota de frequência via comando serial (`CH:<canal>`) com varredura automática caso um nó perca sincronia.

## Pinagem (Arduino Nano + nRF24L01)

| Pino nRF24L01 | Pino Arduino | Observação |
| :--- | :--- | :--- |
| VCC | 3.3V | Recomenda-se capacitor de 10 µF – 100 µF entre 3.3V e GND |
| GND | GND | Terra comum |
| CE | D7 | Chip Enable |
| CSN | D8 | SPI Chip Select |
| SCK | D13 | SPI Clock |
| MOSI | D11 | SPI Master Out |
| MISO | D12 | SPI Master In |
| — | A0 | Entrada analógica (Nó 1) |
| — | D4 | Saída do atuador/LED (Nó 2) |

## Como Executar

### 1. Firmware
Abra `firmware/nrf24_node/nrf24_node.ino`, instale a biblioteca **RF24** (TMRh20) e grave cada placa alterando apenas `#define MEU_ID` (`0`, `1` ou `2`).

### 2. Gateway Web
```bash
cd gateway
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python server.py
