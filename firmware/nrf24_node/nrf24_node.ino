#ifndef MEU_ID
  #define MEU_ID 0  // Padrão caso compile manualmente na IDE
#endif
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

// ================= CONFIGURAÇÃO DO NÓ =================
// Altere APENAS este número antes de gravar cada placa:
// 0 = Gateway (Placa com antena ligada ao PC)
// 1 = Nó Sensor (Lê pino A0 e envia ao Nó 2)
// 2 = Nó Atuador (Controla pino D4 e envia telemetria ao Gateway)
#ifndef MEU_ID
#define MEU_ID 0
#endif

#define CANAL_INICIAL 100
#define PINO_SENSOR A0
#define PINO_ATUADOR 4   // Não usar D13 (SCK do SPI)
// ======================================================

RF24 radio(7, 8); // Pinos CE = 7, CSN = 8
const byte enderecoRede[6] = "IOT26";

enum TipoMsg : uint8_t {
  MSG_BEACON = 1,
  MSG_DADOS_M2M = 2,
  MSG_TELEMETRIA = 3,
  MSG_ACK = 4
};

struct PacoteIoT {
  uint8_t tipo;
  uint8_t origem;
  uint8_t destino;
  uint8_t seq;
  uint8_t canal;
  uint8_t estadoAtuador;
  uint8_t modoAuto;
  uint16_t valorSensor;
  uint16_t limiar;
  uint16_t rpdHits;
  uint16_t retries;
};

uint8_t canalAtual = CANAL_INICIAL;
uint8_t seqAtual = 0;
uint8_t ultimoSeqRecebido = 0;
unsigned long ultimoRxDeduplicacao = 0;

uint8_t estadoAtuador = 0;
uint8_t modoAuto = 1;
uint16_t valorSensor = 0;
uint16_t limiar = 500;

uint16_t totalRpdHits = 0;
uint16_t totalRetries = 0;
uint16_t pacotesRecebidos = 0;

unsigned long ultimoEnvio = 0;
unsigned long ultimoTrafegoRede = 0;
unsigned long janelaBeaconRapidoAte = 0;

void aplicarCanal(uint8_t novoCanal) {
  if (novoCanal > 125) return;
  canalAtual = novoCanal;
  radio.stopListening();
  radio.setChannel(canalAtual);
  radio.startListening();
}

void configurarRadio() {
  radio.setAutoAck(false);
  radio.setRetries(0, 0);
  radio.setDataRate(RF24_250KBPS);
  radio.setCRCLength(RF24_CRC_16);

  // Gateway com antena externa usa PA_MIN para não derrubar os 3.3V nem saturar de perto;
  // Nós remotos usam PA_LOW para atravessar ruído de canais Wi-Fi com estabilidade.
  if (MEU_ID == 0) {
    radio.setPALevel(RF24_PA_MIN);
  } else {
    radio.setPALevel(RF24_PA_LOW);
  }

  radio.setChannel(canalAtual);
  radio.openWritingPipe(enderecoRede);
  radio.openReadingPipe(1, enderecoRede);
  radio.startListening();
}

// Recupera o módulo nRF24 automaticamente caso uma oscilação elétrica resete o chip
void garantirRadioAtivo() {
  if (!radio.isChipConnected() || radio.getChannel() != canalAtual) {
    radio.begin();
    configurarRadio();
  }
}

void atualizarSaidaFisica() {
  if (MEU_ID == 2) {
    digitalWrite(PINO_ATUADOR, estadoAtuador ? HIGH : LOW);
  }
}

// CSMA/CA: escuta portadora (RPD > -64 dBm) antes de transmitir
bool canalLivre() {
  if (radio.available()) return false;
  for (uint8_t i = 0; i < 6; i++) {
    if (radio.testRPD()) {
      totalRpdHits++;
      return false;
    }
    delayMicroseconds(40);
  }
  return true;
}

void enviarRaw(PacoteIoT &pkt) {
  garantirRadioAtivo();
  radio.stopListening();
  delayMicroseconds(130);
  radio.write(&pkt, sizeof(pkt));
  radio.startListening();
}

// Aguarda ACK sem perder comandos de Beacon do Gateway que cheguem no intervalo
bool aguardarAck(uint8_t origemEsperada, uint8_t seqEsperada, uint16_t timeoutMs) {
  unsigned long inicio = millis();
  while (millis() - inicio < timeoutMs) {
    if (radio.available()) {
      PacoteIoT resp;
      radio.read(&resp, sizeof(resp));
      ultimoTrafegoRede = millis();

      if (resp.tipo == MSG_BEACON && resp.origem == 0 && MEU_ID != 0) {
        if (resp.canal != canalAtual && resp.canal <= 125) {
          aplicarCanal(resp.canal);
        }
        modoAuto = resp.modoAuto;
        limiar = resp.limiar;
        if (!modoAuto) {
          estadoAtuador = resp.estadoAtuador;
          atualizarSaidaFisica();
        }
      } else if (resp.tipo == MSG_ACK && resp.destino == MEU_ID &&
                 resp.origem == origemEsperada && resp.seq == seqEsperada) {
        return true;
      }
    }
  }
  return false;
}

// ARQ Stop-and-Wait com Backoff Exponencial
bool enviarConfiavel(PacoteIoT &pkt) {
  pkt.origem = MEU_ID;
  seqAtual++;
  if (seqAtual == 0) seqAtual = 1; // Evita seq 0 após overflow
  pkt.seq = seqAtual;
  pkt.canal = canalAtual;
  pkt.rpdHits = totalRpdHits;
  pkt.retries = totalRetries;

  uint16_t cw = 20;

  for (uint8_t tentativa = 0; tentativa < 5; tentativa++) {
    uint8_t esperasCsma = 0;
    while (!canalLivre() && esperasCsma < 6) {
      delay(random(5, cw));
      esperasCsma++;
    }

    enviarRaw(pkt);

    if (aguardarAck(pkt.destino, pkt.seq, 100)) {
      return true;
    }

    totalRetries++;
    delay(random(15, cw));
    cw = min((uint16_t)(cw * 2), (uint16_t)250);
  }
  return false;
}

void enviarTelemetriaParaGateway() {
  PacoteIoT tel;
  tel.tipo = MSG_TELEMETRIA;
  tel.destino = 0;
  tel.valorSensor = valorSensor;
  tel.estadoAtuador = estadoAtuador;
  tel.modoAuto = modoAuto;
  tel.limiar = limiar;

  bool ok = enviarConfiavel(tel);
  if (MEU_ID == 2) {
    Serial.println(ok ? "[NO 2] TEL -> GW OK" : "[NO 2] FALHA SEM ACK DO GW");
  }
}

// Auto-Scan rápido (0..125) caso o nó fique isolado
void buscarCanalGateway() {
  uint8_t canalAnterior = canalAtual;
  for (uint8_t ch = 0; ch <= 125; ch++) {
    aplicarCanal(ch);
    unsigned long inicio = millis();
    while (millis() - inicio < 25) {
      if (radio.available()) {
        PacoteIoT pkt;
        radio.read(&pkt, sizeof(pkt));
        if (pkt.canal <= 125 && (pkt.tipo == MSG_BEACON || pkt.tipo == MSG_DADOS_M2M || pkt.tipo == MSG_TELEMETRIA)) {
          aplicarCanal(pkt.canal);
          ultimoTrafegoRede = millis();
          return;
        }
      }
    }
  }
  aplicarCanal(canalAnterior);
}

void imprimirJsonGateway() {
  Serial.print("{\"canal\":");
  Serial.print(canalAtual);
  Serial.print(",\"sensor\":");
  Serial.print(valorSensor);
  Serial.print(",\"atuador\":");
  Serial.print(estadoAtuador);
  Serial.print(",\"modoAuto\":");
  Serial.print(modoAuto);
  Serial.print(",\"limiar\":");
  Serial.print(limiar);
  Serial.print(",\"rpdHits\":");
  Serial.print(totalRpdHits);
  Serial.print(",\"retries\":");
  Serial.print(totalRetries);
  Serial.print(",\"rxOk\":");
  Serial.print(pacotesRecebidos);
  Serial.println("}");
}

void dispararBeacon(uint8_t canalAlvo, uint8_t repeticoes) {
  PacoteIoT b;
  b.tipo = MSG_BEACON;
  b.origem = 0;
  b.destino = 255;
  b.seq = ++seqAtual;
  b.canal = canalAlvo;
  b.estadoAtuador = estadoAtuador;
  b.modoAuto = modoAuto;
  b.valorSensor = valorSensor;
  b.limiar = limiar;
  b.rpdHits = totalRpdHits;
  b.retries = totalRetries;

  for (uint8_t i = 0; i < repeticoes; i++) {
    enviarRaw(b);
    if (repeticoes > 1) delay(18);
  }
}

void lerComandosSerialGateway() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.startsWith("CH:")) {
    uint8_t novoCh = cmd.substring(3).toInt();
    if (novoCh <= 125) {
      dispararBeacon(novoCh, 10);
      aplicarCanal(novoCh);
      janelaBeaconRapidoAte = millis() + 5000;
      imprimirJsonGateway();
    }
  } else if (cmd.startsWith("LED:")) {
    modoAuto = 0;
    estadoAtuador = cmd.substring(4).toInt() ? 1 : 0;
    dispararBeacon(canalAtual, 4);
    imprimirJsonGateway();
  } else if (cmd.startsWith("AUTO:")) {
    modoAuto = cmd.substring(5).toInt() ? 1 : 0;
    dispararBeacon(canalAtual, 4);
    imprimirJsonGateway();
  } else if (cmd.startsWith("LIM:")) {
    limiar = cmd.substring(4).toInt();
    dispararBeacon(canalAtual, 4);
    imprimirJsonGateway();
  }
}

bool pacoteNovo(uint8_t seq) {
  if (seq != ultimoSeqRecebido || (millis() - ultimoRxDeduplicacao > 3000)) {
    ultimoSeqRecebido = seq;
    ultimoRxDeduplicacao = millis();
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  if (MEU_ID == 2) {
    pinMode(PINO_ATUADOR, OUTPUT);
    digitalWrite(PINO_ATUADOR, LOW);
  }

  randomSeed(analogRead(A1) + (MEU_ID * 97));

  if (!radio.begin()) {
    Serial.println("{\"erro\":\"nRF24 nao encontrado\"}");
    while (1) {}
  }

  configurarRadio();
  ultimoTrafegoRede = millis();

  if (MEU_ID == 0) {
    imprimirJsonGateway();
  } else {
    Serial.print("BOOT ID=");
    Serial.print(MEU_ID);
    Serial.print(" CHIP_OK=");
    Serial.println(radio.isChipConnected() ? 1 : 0);
  }
}

void loop() {
  garantirRadioAtivo();

  if (radio.available()) {
    PacoteIoT pkt;
    radio.read(&pkt, sizeof(pkt));
    ultimoTrafegoRede = millis();

    // 1. Nós 1 e 2 recebem Beacon do Gateway
    if (pkt.tipo == MSG_BEACON && pkt.origem == 0 && MEU_ID != 0) {
      if (pkt.canal != canalAtual && pkt.canal <= 125) {
        aplicarCanal(pkt.canal);
      }

      uint8_t estadoAnterior = estadoAtuador;
      uint8_t modoAnterior = modoAuto;
      uint16_t limiarAnterior = limiar;

      modoAuto = pkt.modoAuto;
      limiar = pkt.limiar;

      if (!modoAuto) {
        estadoAtuador = pkt.estadoAtuador;
        atualizarSaidaFisica();
      } else if (MEU_ID == 2) {
        estadoAtuador = (valorSensor < limiar) ? 1 : 0;
        atualizarSaidaFisica();
      }

      if (MEU_ID == 2 && (estadoAtuador != estadoAnterior || modoAuto != modoAnterior || limiar != limiarAnterior)) {
        delay(15);
        enviarTelemetriaParaGateway();
      }
    }
    // 2. Nó 2 recebe leitura M2M do Nó 1
    else if (MEU_ID == 2 && pkt.tipo == MSG_DADOS_M2M && pkt.destino == 2) {
      PacoteIoT ack = pkt;
      ack.tipo = MSG_ACK;
      ack.origem = 2;
      ack.destino = pkt.origem;
      delay(2);
      enviarRaw(ack);

      if (pacoteNovo(pkt.seq)) {
        valorSensor = pkt.valorSensor;
        if (modoAuto) {
          estadoAtuador = (valorSensor < limiar) ? 1 : 0;
          atualizarSaidaFisica();
        }
        Serial.println("[NO 2] RX do NO 1 -> enviando ao GW");
        delay(15);
        enviarTelemetriaParaGateway();
      }
    }
    // 3. Gateway recebe telemetria consolidada do Nó 2
    else if (MEU_ID == 0 && pkt.tipo == MSG_TELEMETRIA && pkt.destino == 0) {
      PacoteIoT ack = pkt;
      ack.tipo = MSG_ACK;
      ack.origem = 0;
      ack.destino = pkt.origem;
      delay(2);
      enviarRaw(ack);

      if (pacoteNovo(pkt.seq)) {
        valorSensor = pkt.valorSensor;
        estadoAtuador = pkt.estadoAtuador;
        modoAuto = pkt.modoAuto;
        limiar = pkt.limiar;
        totalRpdHits = pkt.rpdHits;
        totalRetries = pkt.retries;
        pacotesRecebidos++;

        imprimirJsonGateway();
      }
    }
  }

  if (MEU_ID == 0) {
    lerComandosSerialGateway();
    unsigned long intervaloBeacon = (millis() < janelaBeaconRapidoAte) ? 80 : 1500;
    if (millis() - ultimoEnvio >= intervaloBeacon) {
      ultimoEnvio = millis();
      dispararBeacon(canalAtual, 1);
    }
  } else {
    // Re-sincroniza automaticamente se ficar 5s sem ouvir tráfego no canal atual
    if (millis() - ultimoTrafegoRede > 5000) {
      buscarCanalGateway();
      ultimoTrafegoRede = millis();
    }

    if (MEU_ID == 1 && (millis() - ultimoEnvio >= 1200)) {
      ultimoEnvio = millis();
      PacoteIoT m2m;
      m2m.tipo = MSG_DADOS_M2M;
      m2m.destino = 2;
      m2m.valorSensor = analogRead(PINO_SENSOR);

      bool ok = enviarConfiavel(m2m);
      Serial.println(ok ? "[NO 1] M2M -> NO 2 OK" : "[NO 1] FALHA SEM ACK DO NO 2");
    }
  }
}
