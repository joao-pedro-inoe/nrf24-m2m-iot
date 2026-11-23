/**
 * ============================================================================
 * UTFPR • OPT101 — Firmware de Rede Sem Fio IoT / M2M com nRF24L01+
 * ============================================================================
 * Arquivo: nrf24_node.ino
 * Linguagem: C++ (Dialeto Arduino / Microcontrolador AVR ATmega328P)
 * Transceptor de Rádio: Nordic Semiconductor nRF24L01+ (Faixa ISM de 2.4 GHz)
 *
 * ----------------------------------------------------------------------------
 * GUIA DIDÁTICO PARA INICIANTES: O QUE É ESTE PROJETO?
 * ----------------------------------------------------------------------------
 * Imagine um sistema inteligente de automação predial ou industrial:
 * Temos três placas Arduino conversando pelo ar, sem nenhum fio entre elas:
 *
 *   [ Placa 1: Sensor ]  -------- (Rádio M2M) -------->  [ Placa 2: Atuador ]
 *     Lê luminosidade                                      Liga/desliga a carga
 *     (Pino A0)                                            (Pino D4: LED/Relé)
 *                                                                  |
 *                                                           (Telemetria)
 *                                                                  |
 *                                                                  v
 *                                                       [ Placa 0: Gateway ]
 *                                                          Ligada via USB ao PC
 *                                                          (Exibe painel Web)
 *
 * POR QUE ISSO SE CHAMA "M2M" (MACHINE-TO-MACHINE)?
 * "Machine-to-Machine" significa comunicação direta "Máquina para Máquina".
 * O Nó 1 (Sensor) manda as informações direto para o Nó 2 (Atuador), sem
 * precisar pedir autorização a um computador ou servidor central. Se o PC do
 * Gateway for desligado, a luz do Nó 2 continua acendendo e apagando normalmente!
 *
 * POR QUE O MESMO CÓDIGO É GRAVADO NAS 3 PLACAS?
 * Para facilitar a manutenção! Em vez de termos 3 arquivos de código diferentes,
 * temos um único arquivo. A placa sabe qual papel desempenhar simplesmente lendo
 * a constante `MEU_ID` (0 = Gateway, 1 = Sensor, 2 = Atuador).
 *
 * ----------------------------------------------------------------------------
 * CONCEITOS IMPORTANTES DE REDES EXPLICADOS DE FORMA SIMPLES:
 * ----------------------------------------------------------------------------
 * 1. O QUE É UM PACOTE DE DADOS?
 *    Uma "carta" digital transmitida pelo ar. Em vez de papel, são 15 bytes
 *    (números de 0 a 255) contendo quem enviou, para quem é, qual o valor do
 *    sensor e qual o comando.
 *
 * 2. O QUE É CSMA/CA (Carrier Sense Multiple Access / Collision Avoidance)?
 *    É a "regra de boa educação" do rádio. Antes de transmitir, a placa "escuta"
 *    o ar para ver se já tem outro rádio transmitindo. Se o ar estiver ocupado,
 *    ela espera um instante aleatório antes de tentar de novo. Isso evita que
 *    duas placas falem juntas e embaralhem o sinal (colisão).
 *
 * 3. O QUE É ACK (Reconhecimento / Aviso de Recebimento)?
 *    É como o tique azul do WhatsApp ou o recibo de uma carta registrada.
 *    Quem envia quer ter certeza de que a mensagem chegou. O destinatário recebe
 *    e devolve imediatamente um pacote curto dizendo: "Recebi com sucesso!".
 *
 * 4. O QUE É STOP-AND-WAIT COM BACKOFF EXPONENCIAL?
 *    Se o remetente enviar uma mensagem e não receber o "recibo" (ACK) em até
 *    100 milissegundos, ele deduz que a mensagem se perdeu no ruído. Ele então
 *    sorteia um tempo de espera para tentar novamente. A cada tentativa que
 *    falha, o tempo máximo de espera dobra (20ms, 40ms, 80ms...).
 *
 * 5. O QUE É O BEACON (Farol de Sincronismo)?
 *    O Gateway funciona como um "farol marítimo" girando na costa: a cada 1,5s
 *    ele transmite uma mensagem para todos (Broadcast) dizendo: "Pessoal,
 *    estamos operando no canal X, com limiar Y e no modo Z". Se o operador no PC
 *    mudar alguma configuração, todos os nós aprendem pelo Beacon.
 *
 * 6. O QUE É DEDUPLICAÇÃO?
 *    Se o Nó 1 mandou uma leitura, o Nó 2 executou e mandou o recibo (ACK),
 *    mas uma interferência destruiu o recibo no ar, o Nó 1 vai achar que falhou
 *    e mandará a mensagem de novo. O Nó 2 percebe pelo número de sequência (seq)
 *    que é uma mensagem repetida, confirma o recibo, mas NÃO reexecuta a ação.
 * ============================================================================
 */

// Inclusão das bibliotecas necessárias:
// SPI.h: Permite a comunicação rápida do microcontrolador com periféricos via barramento SPI
// nRF24L01.h e RF24.h: Driver oficial para configurar e operar o chip de rádio nRF24L01+
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

// ============================================================================
// CONFIGURAÇÃO DA IDENTIDADE DA PLACA (MEU_ID)
// ============================================================================
// Altere este número antes de gravar cada Arduino, ou deixe a ferramenta de linha
// de comando (CLI) definir automaticamente via flag de compilação (-DMEU_ID=X):
//
//   0 = GATEWAY: Fica plugado no cabo USB do computador, repassando dados em JSON
//   1 = SENSOR:  Lê o pino analógico A0 e transmite via rádio para o Nó 2
//   2 = ATUADOR: Ouve o Nó 1, aciona o pino digital D4 e reporta tudo ao Gateway
#ifndef MEU_ID
  #define MEU_ID 0  // Valor padrão caso o código seja aberto e gravado direto pela Arduino IDE
#endif

// ============================================================================
// MAPEAMENTO FÍSICO DE PINOS E FREQUÊNCIA DE OPERAÇÃO
// ============================================================================

// Canal inicial de rádio (Canais vão de 0 a 125, onde cada canal salta 1 MHz).
// Canal 100 = 2.400 GHz + 100 MHz = 2.500 GHz.
// Escolhemos 2.500 GHz porque fica ACIMA dos canais convencionais de Wi-Fi doméstico (2.412 a 2.472 GHz),
// evitando que o roteador de internet da sala interfira na comunicação dos Arduinos.
#define CANAL_INICIAL 100

// Pino Analógico A0: Conectado ao sensor (ex: LDR para luz, potenciômetro ou termistor).
// Um pino analógico lê voltagens contínuas entre 0V e 5V, transformando em números de 0 a 1023.
#define PINO_SENSOR A0

// Pino Digital D4: Conectado ao dispositivo atuador (ex: módulo relé, transistor ou LED indicador).
// ATENÇÃO DE ENGENHARIA: Não usamos o pino digital D13! No Arduino Uno e Nano, o pino D13
// é conectado ao pino SCK (Serial Clock) do rádio nRF24 via SPI. Se usássemos o pino 13 para
// o atuador, haveria curto lógico e a comunicação do rádio travaria imediatamente.
#define PINO_ATUADOR 4

// ============================================================================
// CRIAÇÃO DO OBJETO DE RÁDIO E ENDEREÇO DA REDE
// ============================================================================
// Instancia o rádio nos pinos de controle:
//   Pino 7 = CE (Chip Enable): "Liga/desliga" o modo de transmissão e escuta do rádio.
//   Pino 8 = CSN (Chip Select Not): Avisa o rádio quando o Arduino vai conversar com ele via SPI.
RF24 radio(7, 8);

// Nome da "sala" ou "endereço" de rádio compartilhado por todas as 3 placas na mesma rede.
// São 5 letras ("IOT26") seguidas do caractere nulo '\0' terminador em C.
const byte enderecoRede[6] = "IOT26";

// ============================================================================
// DICIONÁRIO DE TIPOS DE MENSAGENS (TIPO DO PACOTE)
// ============================================================================
// Um número de 1 byte que vai no início de cada pacote para o receptor saber o que fazer:
enum TipoMsg : uint8_t {
  MSG_BEACON     = 1, // Farol emitido pelo Gateway para anunciar canal e sincronizar os nós
  MSG_DADOS_M2M  = 2, // Leitura do sensor enviada diretamente do Nó 1 para o Nó 2
  MSG_TELEMETRIA = 3, // Relatório geral emitido pelo Nó 2 para atualizar o Gateway
  MSG_ACK        = 4  // Recibo de confirmação dizendo: "Recebi seu pacote sem erros!"
};

// ============================================================================
// ANATOMIA DO PACOTE IOT (O FORMATO DA CARTA DIGITAL)
// ============================================================================
// Esta estrutura em C define exatamente como os 15 bytes de dados são organizados no ar.
// Todos os nós utilizam essa mesma estrutura para empacotar e desempacotar as mensagens:
struct PacoteIoT {
  uint8_t  tipo;           // 1 Byte:  Qual o tipo da mensagem (MSG_BEACON, MSG_DADOS_M2M, etc.)
  uint8_t  origem;         // 1 Byte:  Quem enviou o pacote? (0 = Gateway, 1 = Sensor, 2 = Atuador)
  uint8_t  destino;        // 1 Byte:  Quem deve receber? (0, 1, 2 ou 255 se for para todos/Broadcast)
  uint8_t  seq;            // 1 Byte:  Número da sequência (1, 2, 3...) usado para evitar mensagens duplicadas
  uint8_t  canal;          // 1 Byte:  Canal de frequência no qual o transmissor está trabalhando (0 a 125)
  uint8_t  estadoAtuador;  // 1 Byte:  Estado da carga (0 = Desligado, 1 = Ligado)
  uint8_t  modoAuto;       // 1 Byte:  Modo de operação (1 = Automático via sensor, 0 = Manual via PC)
  uint16_t valorSensor;    // 2 Bytes: Leitura analógica de 10 bits do sensor (0 a 1023)
  uint16_t limiar;         // 2 Bytes: Valor de corte para o modo automático (ex: abaixo de 500 liga o LED)
  uint16_t rpdHits;        // 2 Bytes: Contador de vezes que o rádio tentou falar mas achou o ar ocupado
  uint16_t retries;        // 2 Bytes: Contador de quantas retransmissões foram necessárias por falta de ACK
}; // Tamanho total somado: 1+1+1+1+1+1+1+2+2+2+2 = 15 Bytes (cabe com folga no limite de 32B do nRF24L01)

// ============================================================================
// VARIÁVEIS GLOBAIS DE ESTADO (A MEMÓRIA DO ARDUINO)
// ============================================================================

// Canal em uso no momento pela placa (começa em 100, mas pode mudar dinamicamente)
uint8_t canalAtual = CANAL_INICIAL;

// Sequenciador de pacotes transmitidos: incrementa de 1 em 1 a cada nova mensagem enviada
uint8_t seqAtual = 0;

// Filtro de mensagens repetidas: guarda o número da última mensagem processada com sucesso
uint8_t ultimoSeqRecebido = 0;

// Marcação de tempo da última mensagem processada (para reiniciar o filtro a cada 3 segundos)
unsigned long ultimoRxDeduplicacao = 0;

// Variáveis de controle do atuador e do processo físico
uint8_t  estadoAtuador = 0;              // 0 = Relé/LED desligado, 1 = Ligado
uint8_t  modoAuto = 1;                   // 1 = Modo automático (sensor decide), 0 = Manual (usuário manda)
uint16_t valorSensor = 0;                // Valor mais recente medido no sensor de luminosidade
uint16_t limiar = 500;                   // Ponto de transição padrão: se valorSensor < 500, liga a luz

// Estatísticas de qualidade de comunicação e telemetria
uint16_t totalRpdHits = 0;               // Quantas vezes o ar estava ocupado (interferência detectada)
uint16_t totalRetries = 0;               // Quantas vezes tivemos que reenviar por falta de recibo
uint16_t pacotesRecebidos = 0;           // Quantos pacotes válidos chegaram nesta placa

// Relógios de controle não-bloqueantes baseados na função millis()
unsigned long ultimoEnvio = 0;           // Guarda em que milissegundo fizemos o último envio periódico
unsigned long ultimoTrafegoRede = 0;     // Guarda em que milissegundo ouvimos tráfego legítimo pela última vez
unsigned long janelaBeaconRapidoAte = 0; // Temporizador para emitir beacons velozes ao trocar de canal

// ============================================================================
// FUNÇÃO: aplicarCanal(novoCanal)
// ----------------------------------------------------------------------------
// O QUE FAZ: Sintoniza o módulo de rádio em uma nova frequência de trabalho,
// exatamente como mudar a estação de um rádio FM.
// COMO FAZ: O chip nRF24L01 não permite mudar a frequência enquanto está com o
// receptor ligado. Portanto:
//   1. Desliga o receptor (stopListening).
//   2. Muda o registrador de frequência (setChannel).
//   3. Liga novamente o receptor no novo canal (startListening).
// ============================================================================
void aplicarCanal(uint8_t novoCanal) {
  // Canais válidos no chip nRF24L01 vão de 0 a 125. Qualquer valor acima é ignorado por segurança.
  if (novoCanal > 125) return;

  canalAtual = novoCanal;
  radio.stopListening();         // Pausa a escuta para permitir reconfigurar os circuitos internos de PLL
  radio.setChannel(canalAtual);  // Grava o novo canal de rádio
  radio.startListening();        // Reativa a antena no modo receptor contínuo
}

// ============================================================================
// FUNÇÃO: configurarRadio()
// ----------------------------------------------------------------------------
// O QUE FAZ: Ajusta todas as propriedades elétricas e de comunicação do chip nRF24.
// POR QUE DESATIVAMOS O AUTO-ACK DE HARDWARE?
// O nRF24L01 possui um recurso nativo chamado Enhanced ShockBurst (Auto-Ack). Porém,
// para finalidades didáticas e controle total de colisões, nós mesmos implementamos
// o envio de ACK, o CSMA/CA e o Backoff em software no código C++.
// POR QUE AS POTÊNCIAS DE TRANSMISSÃO SÃO DIFERENTES?
// - Gateway (ID 0): Fica ligado na porta USB do PC e costuma ter antena externa amplificada.
//   Usamos PA_MIN (-18 dBm) para não sobrecarregar o regulador de 3.3V do Arduino e para
//   não "ensurdecer" os nós vizinhos que estão perto na mesma bancada.
// - Nós remotos (IDs 1 e 2): Ficam distantes e usam PA_LOW (-12 dBm) para que o sinal
//   consiga furar obstáculos e ruídos de Wi-Fi sem gastar energia excessiva.
// ============================================================================
void configurarRadio() {
  radio.setAutoAck(false);          // Desativa o ACK automático do chip (nós controlamos via software)
  radio.setRetries(0, 0);           // Desativa repetições de hardware (nós controlamos nosso próprio ARQ)
  radio.setDataRate(RF24_250KBPS);  // Velocidade de 250 kbps: a menor velocidade garante o maior alcance!
  radio.setCRCLength(RF24_CRC_16);  // Verificação de erros por CRC de 16 bits (descarta pacotes corrompidos)

  // Ajusta a potência de emissão de acordo com o papel do nó
  if (MEU_ID == 0) {
    radio.setPALevel(RF24_PA_MIN);  // Potência mínima no Gateway para proteger a USB e evitar saturação
  } else {
    radio.setPALevel(RF24_PA_LOW);  // Potência moderada nos nós remotos
  }

  radio.setChannel(canalAtual);            // Define a frequência de partida (Canal 100)
  radio.openWritingPipe(enderecoRede);     // Configura o endereço para onde os pacotes serão transmitidos
  radio.openReadingPipe(1, enderecoRede);  // Configura a caixa de entrada para receber pacotes com esse endereço
  radio.startListening();                  // Coloca o rádio em prontidão para receber dados
}

// ============================================================================
// FUNÇÃO: garantirRadioAtivo() [Cão de Guarda / Watchdog do Rádio]
// ----------------------------------------------------------------------------
// O QUE FAZ: Protege o sistema contra quedas de energia e ruídos elétricos.
// EXPLICAÇÃO DIDÁTICA: O módulo nRF24 é muito sensível a pequenas quedas de tensão
// no pino 3.3V do Arduino (quando um relé atraca ou um motor liga, por exemplo).
// Uma piscadinha na alimentação pode fazer o chip do rádio reiniciar e perder
// todas as configurações. Esta função confere se o chip ainda está respondendo na SPI;
// se ele tiver "apagado", ela reinicia e reconfigura o rádio no mesmo milissegundo!
// ============================================================================
void garantirRadioAtivo() {
  if (!radio.isChipConnected() || radio.getChannel() != canalAtual) {
    radio.begin();         // Reinicia a interface SPI com o chip
    configurarRadio();     // Reaplica todas as configurações de canais e taxas
  }
}

// ============================================================================
// FUNÇÃO: atualizarSaidaFisica()
// ----------------------------------------------------------------------------
// O QUE FAZ: Aciona ou desliga a saída de controle (Pino D4) no mundo real.
// DETALHE: Esta função só faz efeito físico se a placa for o Nó Atuador (MEU_ID == 2).
// Se estadoAtuador for 1, coloca 5V no pino D4 (HIGH). Se for 0, coloca 0V (LOW).
// ============================================================================
void atualizarSaidaFisica() {
  if (MEU_ID == 2) {
    digitalWrite(PINO_ATUADOR, estadoAtuador ? HIGH : LOW);
  }
}

// ============================================================================
// FUNÇÃO: canalLivre() [Implementação do CSMA/CA]
// ----------------------------------------------------------------------------
// O QUE FAZ: Funciona como uma pessoa que quer falar em uma reunião: ela para,
// escuta por alguns microssegundos e só fala se ninguém estiver falando.
// DETALHE TÉCNICO: O chip nRF24L01 possui um registrador interno chamado RPD
// (Received Power Detector). Ele acusa se há ondas eletromagnéticas no canal com
// potência maior que -64 dBm.
// Retorna true se o canal estiver livre, ou false se alguém estiver transmitindo.
// ============================================================================
bool canalLivre() {
  // 1. Se já chegou algum pacote na nossa caixa de entrada (FIFO), o canal com certeza não está vago
  if (radio.available()) return false;

  // 2. Fazemos 6 checagens rápidas no medidor de potência RPD, esperando 40 microssegundos entre elas
  for (uint8_t i = 0; i < 6; i++) {
    if (radio.testRPD()) {
      totalRpdHits++; // Encontrou portadora de sinal no ar: soma no contador de interferências
      return false;   // Canal ocupado! Não transmita agora para evitar colisão
    }
    delayMicroseconds(40); // Pequena pausa antes da próxima verificação
  }

  return true; // Se passou em todos os testes, o canal está silencioso e limpo!
}

// ============================================================================
// FUNÇÃO: enviarRaw(pkt) [Envio em Baixo Nível]
// ----------------------------------------------------------------------------
// O QUE FAZ: Dispara fisicamente o pacote de 15 bytes pela antena sem esperar resposta.
// TEMPO DE PLL (130 µs): O transmissor do rádio precisa de exatamente 130 microssegundos
// para estabilizar seu oscilador de frequência (PLL) ao mudar do modo de escuta para
// o modo de transmissão. Sem essa pausa, os primeiros bits sairiam cortados!
// ============================================================================
void enviarRaw(PacoteIoT &pkt) {
  garantirRadioAtivo();           // Confere se o rádio está vivo antes de tentar falar
  radio.stopListening();          // Sai do modo de escuta (RX) e entra em prontidão para transmissão (TX)
  delayMicroseconds(130);         // Tempo obrigatório do fabricante para travamento estável do circuito PLL
  radio.write(&pkt, sizeof(pkt)); // Empurra os 15 bytes para a antena
  radio.startListening();         // Volta imediatamente a escutar (RX) para não perder nenhuma mensagem
}

// ============================================================================
// FUNÇÃO: aguardarAck(origemEsperada, seqEsperada, timeoutMs)
// ----------------------------------------------------------------------------
// O QUE FAZ: Fica esperando a chegada de um pacote do tipo MSG_ACK (o "recibo")
// enviado pela placa de destino durante um limite de tempo (timeoutMs).
// COMPORTAMENTO INTELIGENTE: Enquanto espera o recibo, se o Gateway emitir um
// pacote Beacon com ordens de emergência (ex: mudar de canal ou desligar carga),
// o nó aproveita a viagem para processar o Beacon e não ficar surdo a novos comandos!
// ============================================================================
bool aguardarAck(uint8_t origemEsperada, uint8_t seqEsperada, uint16_t timeoutMs) {
  unsigned long inicio = millis(); // Guarda o momento exato em que começamos a esperar

  // Enquanto não passar o tempo limite (ex: 100 milissegundos)...
  while (millis() - inicio < timeoutMs) {
    if (radio.available()) {
      PacoteIoT resp;
      radio.read(&resp, sizeof(resp)); // Tira a mensagem do rádio e lê
      ultimoTrafegoRede = millis();   // Avisa o sistema que a rede está viva

      // Se a mensagem for um Beacon do Gateway, processamos na hora!
      if (resp.tipo == MSG_BEACON && resp.origem == 0 && MEU_ID != 0) {
        if (resp.canal != canalAtual && resp.canal <= 125) {
          aplicarCanal(resp.canal); // Acompanha a migração de frequência do Gateway
        }
        modoAuto = resp.modoAuto;
        limiar = resp.limiar;
        if (!modoAuto) {
          estadoAtuador = resp.estadoAtuador;
          atualizarSaidaFisica();
        }
      }
      // Se a mensagem for o ACK exato que estávamos esperando (mesmo remetente e mesmo número de seq):
      else if (resp.tipo == MSG_ACK && resp.destino == MEU_ID &&
               resp.origem == origemEsperada && resp.seq == seqEsperada) {
        return true; // Sucesso! O recibo chegou e a transação está concluída!
      }
    }
  }

  return false; // O tempo estourou e o recibo não veio (possível perda ou colisão de rádio)
}

// ============================================================================
// FUNÇÃO: enviarConfiavel(pkt) [Transporte Confiável com Backoff Exponencial]
// ----------------------------------------------------------------------------
// O QUE FAZ: É a garantia de entrega da nossa rede.
// FUNCIONAMENTO PASSO A PASSO:
//   1. Carrega no pacote quem somos (MEU_ID), o novo número de sequência (seqAtual)
//      e as estatísticas de desempenho.
//   2. Tenta transmitir até 5 vezes.
//   3. Em cada tentativa, usa a função canalLivre() para não falar por cima dos outros.
//   4. Se o canal estiver ocupado, espera um sorteio aleatório (Jitter).
//   5. Transmite e espera o recibo (ACK) por 100ms.
//   6. Se não receber o recibo, duplica o tempo máximo da janela de sorteio (Backoff
//      Exponencial: 20ms -> 40ms -> 80ms -> 160ms -> 250ms) e tenta de novo.
// ============================================================================
bool enviarConfiavel(PacoteIoT &pkt) {
  pkt.origem = MEU_ID;

  // Gera um número sequencial único para este pacote (1 a 255)
  seqAtual++;
  if (seqAtual == 0) seqAtual = 1; // Se der a volta de 255 para 0, pula o zero (zero é reservado para boot)
  pkt.seq = seqAtual;

  pkt.canal = canalAtual;
  pkt.rpdHits = totalRpdHits;
  pkt.retries = totalRetries;

  uint16_t cw = 20; // Contention Window: janela inicial de sorteio de espera (20 milissegundos)

  // Fazemos até 5 tentativas de envio antes de desistir
  for (uint8_t tentativa = 0; tentativa < 5; tentativa++) {
    // Escuta o ar: se estiver ocupado, aguarda um tempo randômico até 6 vezes
    uint8_t esperasCsma = 0;
    while (!canalLivre() && esperasCsma < 6) {
      delay(random(5, cw)); // Sorteia um tempo de pausa entre 5ms e cw para desempatar
      esperasCsma++;
    }

    // Transmite fisicamente o pacote pela antena
    enviarRaw(pkt);

    // Aguarda até 100 milissegundos pelo recibo (ACK) vindo do destinatário
    if (aguardarAck(pkt.destino, pkt.seq, 100)) {
      return true; // Recebido e confirmado com sucesso!
    }

    // Se chegou aqui, não houve recibo: contabiliza a tentativa e dobra a janela de espera
    totalRetries++;
    delay(random(15, cw));                       // Espera um tempo antes de tentar a próxima rodada
    cw = min((uint16_t)(cw * 2), (uint16_t)250); // Dobra a janela de contenção (teto máximo de 250ms)
  }

  return false; // Esgotou as 5 tentativas sem confirmação (nó de destino inacessível)
}

// ============================================================================
// FUNÇÃO: enviarTelemetriaParaGateway()
// ----------------------------------------------------------------------------
// O QUE FAZ: O Nó Atuador (ID 2) empacota tudo o que está acontecendo (leitura
// que recebeu do sensor, se a luz está acesa ou apagada, se está no modo automático)
// e despacha para o Gateway (ID 0) com garantia de entrega (enviarConfiavel).
// ============================================================================
void enviarTelemetriaParaGateway() {
  PacoteIoT tel;
  tel.tipo = MSG_TELEMETRIA;      // Tipo 3: Telemetria
  tel.destino = 0;                 // O destinatário final é o Gateway (ID 0)
  tel.valorSensor = valorSensor;   // Valor analógico lido
  tel.estadoAtuador = estadoAtuador;// Estado da lâmpada/relé
  tel.modoAuto = modoAuto;         // Modo automático ou manual
  tel.limiar = limiar;             // Ponto de corte configurado

  bool ok = enviarConfiavel(tel);
  if (MEU_ID == 2) {
    // Imprime na porta serial local para podermos acompanhar pelo monitor do Arduino
    Serial.println(ok ? "[NO 2] TEL -> GW OK" : "[NO 2] FALHA SEM ACK DO GW");
  }
}

// ============================================================================
// FUNÇÃO: buscarCanalGateway() [Varredura Automática / Auto-Scan]
// ----------------------------------------------------------------------------
// O QUE FAZ: É o "resgate automático" de nós perdidos!
// SITUAÇÃO: Imagine que o operador no computador mandou o Gateway mudar do Canal 100
// para o Canal 110. Se o Nó 1 estava temporariamente desligado durante a ordem, ele
// vai ligar e ficar sozinho no Canal 100, sem ouvir ninguém.
// SOLUÇÃO: Se um nó passar mais de 5 segundos em silêncio absoluto, esta função é
// disparada. Ela passa de canal em canal (do canal 0 até o 125), ouvindo 25 milissegundos
// em cada um. Quando ela escutar qualquer pacote da nossa rede, ela "trava" naquele canal
// e volta a funcionar como se nada tivesse acontecido!
// ============================================================================
void buscarCanalGateway() {
  uint8_t canalAnterior = canalAtual;

  // Varre os 126 canais possíveis do espectro de 2.4 GHz
  for (uint8_t ch = 0; ch <= 125; ch++) {
    aplicarCanal(ch);
    unsigned long inicio = millis();

    // Fica em silêncio escutando o canal por 25 milissegundos
    while (millis() - inicio < 25) {
      if (radio.available()) {
        PacoteIoT pkt;
        radio.read(&pkt, sizeof(pkt));

        // Se encontrou qualquer mensagem legítima da rede (Beacon, M2M ou Telemetria):
        if (pkt.canal <= 125 && (pkt.tipo == MSG_BEACON || pkt.tipo == MSG_DADOS_M2M || pkt.tipo == MSG_TELEMETRIA)) {
          aplicarCanal(pkt.canal);    // Encontrou a rede! Sintoniza em definitivo neste canal
          ultimoTrafegoRede = millis();
          return;                     // Sai da varredura com sucesso!
        }
      }
    }
  }

  // Se varreu todos os 126 canais e não ouviu ninguém, volta para o canal onde estava
  aplicarCanal(canalAnterior);
}

// ============================================================================
// FUNÇÃO: imprimirJsonGateway()
// ----------------------------------------------------------------------------
// O QUE FAZ: Usada exclusivamente pelo Gateway (MEU_ID == 0).
// Pega todas as variáveis da rede e imprime na porta USB em formato JSON legível
// por computadores (ex: {"canal":100,"sensor":420,...}).
// O servidor em Python (server.py) lê essas linhas de texto e atualiza o site na tela!
// ============================================================================
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

// ============================================================================
// FUNÇÃO: dispararBeacon(canalAlvo, repeticoes)
// ----------------------------------------------------------------------------
// O QUE FAZ: O Gateway (Nó 0) transmite uma mensagem de "Aviso Geral" (Broadcast
// para o endereço 255) contendo o canal oficial, estado desejado do relé e limiar.
// PARÂMETRO repeticoes: Ao trocar de canal, enviamos uma rajada de 10 beacons em
// sequência rápida para garantir que mesmo com ruído os nós recebam o aviso de mudança.
// ============================================================================
void dispararBeacon(uint8_t canalAlvo, uint8_t repeticoes) {
  PacoteIoT b;
  b.tipo = MSG_BEACON;
  b.origem = 0;                    // Sempre parte do Gateway (ID 0)
  b.destino = 255;                  // 255 é o endereço universal de "Atenção Todos" (Broadcast)
  b.seq = ++seqAtual;               // Incrementa o número da mensagem
  b.canal = canalAlvo;              // Anuncia qual canal todos devem adotar
  b.estadoAtuador = estadoAtuador;  // Informa o estado atual do atuador
  b.modoAuto = modoAuto;            // Informa se estamos em modo manual ou automático
  b.valorSensor = valorSensor;      // Informa o último valor de sensor conhecido
  b.limiar = limiar;                // Informa o limiar de comparação
  b.rpdHits = totalRpdHits;
  b.retries = totalRetries;

  for (uint8_t i = 0; i < repeticoes; i++) {
    enviarRaw(b);
    if (repeticoes > 1) delay(18); // Pequena folga de 18ms entre rajadas repetidas
  }
}

// ============================================================================
// FUNÇÃO: lerComandosSerialGateway()
// ----------------------------------------------------------------------------
// O QUE FAZ: O Gateway fica "lendo" as mensagens que chegam do computador pelo cabo USB.
// O usuário pode digitar comandos no terminal ou clicar em botões no painel Web:
//   - "CH:105"   -> Manda toda a rede mudar para o canal de frequência 105
//   - "LED:1"    -> Força o atuador a ligar (passa automaticamente para modo Manual)
//   - "LED:0"    -> Força o atuador a desligar
//   - "AUTO:1"   -> Devolve o controle ao sensor automático
//   - "LIM:600"  -> Ajusta a sensibilidade de corte para 600
// ============================================================================
void lerComandosSerialGateway() {
  if (!Serial.available()) return;          // Se o PC não mandou nada, encerra imediatamente
  String cmd = Serial.readStringUntil('\n'); // Lê a linha de texto até o "Enter"
  cmd.trim();                               // Remove espaços em branco ou quebras de linha extras

  // Comando para mudar canal de rádio
  if (cmd.startsWith("CH:")) {
    uint8_t novoCh = cmd.substring(3).toInt(); // Extrai o número após "CH:"
    if (novoCh <= 125) {
      // 1. Avisa os outros nós com uma rajada de 10 beacons no canal antigo: "Atenção, vamos mudar!"
      dispararBeacon(novoCh, 10);
      // 2. Agora sintoniza o rádio do próprio Gateway no canal novo
      aplicarCanal(novoCh);
      // 3. Pelos próximos 5 segundos, dispara beacons bem rápidos (a cada 80ms) para ajudar quem atrasou
      janelaBeaconRapidoAte = millis() + 5000;
      imprimirJsonGateway(); // Atualiza a tela do computador
    }
  }
  // Comando para ligar ou desligar o atuador manualmente
  else if (cmd.startsWith("LED:")) {
    modoAuto = 0;                                         // Desativa o modo automático
    estadoAtuador = cmd.substring(4).toInt() ? 1 : 0;     // 1 = liga, 0 = desliga
    dispararBeacon(canalAtual, 4);                         // Espalha a ordem para o Nó 2
    imprimirJsonGateway();
  }
  // Comando para reativar o modo automático
  else if (cmd.startsWith("AUTO:")) {
    modoAuto = cmd.substring(5).toInt() ? 1 : 0;
    dispararBeacon(canalAtual, 4);
    imprimirJsonGateway();
  }
  // Comando para trocar o valor de limiar
  else if (cmd.startsWith("LIM:")) {
    limiar = cmd.substring(4).toInt();
    dispararBeacon(canalAtual, 4);
    imprimirJsonGateway();
  }
}

// ============================================================================
// FUNÇÃO: pacoteNovo(seq) [Algoritmo de Deduplicação]
// ----------------------------------------------------------------------------
// O QUE FAZ: Garante que uma mensagem repetida não seja executada duas vezes.
// ANALOGIA DO CARTEIRO: Se o carteiro entregar a mesma carta duas vezes porque
// achou que a primeira tinha se perdido na chuva, você não quer pagar a mesma
// conta duas vezes! Você olha o número de controle: se for idêntico ao anterior
// e tiver chegado a menos de 3 segundos, você ignora a carta repetida.
// Retorna true se a mensagem for inédita; false se for repetição inútil.
// ============================================================================
bool pacoteNovo(uint8_t seq) {
  // Se o número de sequência for diferente do último, OU se já se passaram mais de 3 segundos
  if (seq != ultimoSeqRecebido || (millis() - ultimoRxDeduplicacao > 3000)) {
    ultimoSeqRecebido = seq;         // Memoriza este novo número
    ultimoRxDeduplicacao = millis(); // Atualiza o relógio de deduplicação
    return true;                     // Pacote novo aprovado!
  }
  return false; // Pacote repetido descartado!
}

// ============================================================================
// FUNÇÃO: setup() [Configuração Inicial do Arduino]
// ----------------------------------------------------------------------------
// O QUE FAZ: O Arduino executa o setup() apenas UMA VEZ, assim que é ligado na tomada
// ou recebe um comando de reset. Aqui preparamos pinos, comunicação e o rádio.
// ============================================================================
void setup() {
  // Inicia a comunicação serial USB a 115200 bits por segundo (alta velocidade)
  Serial.begin(115200);
  Serial.setTimeout(50); // Não trava mais que 50ms esperando leituras de texto

  // Se esta placa for o Nó Atuador (ID 2), preparamos o pino D4 como saída de força
  if (MEU_ID == 2) {
    pinMode(PINO_ATUADOR, OUTPUT);
    digitalWrite(PINO_ATUADOR, LOW); // Começa com o relé/LED desligado por segurança
  }

  // TRUQUE DA SEMENTE ALEATÓRIA (randomSeed):
  // Arduinos são computadores lógicos: sem ajuda externa, a função random() geraria
  // sempre exatamente a mesma sequência de números. Se ligássemos os 3 nós ao mesmo
  // tempo na mesma régua elétrica, todos sorteariam os mesmos números e colidiriam juntos!
  // SOLUÇÃO: Lemos a voltagem estática do pino analógico A1 (que está desconectado,
  // funcionando como uma antena de ruído eletromagnético do ambiente) e somamos com
  // o número exclusivo de cada nó (MEU_ID * 97). Agora cada placa tem sorteios únicos!
  randomSeed(analogRead(A1) + (MEU_ID * 97));

  // Tenta ligar o chip de rádio nRF24L01 via barramento SPI
  if (!radio.begin()) {
    // Se o rádio estiver mal encaixado, com fio solto ou sem energia, avisa e trava
    Serial.println("{\"erro\":\"nRF24 nao encontrado\"}");
    while (1) {} // Trava aqui em loop infinito para não executar rotinas sem rádio
  }

  // Aplica as nossas configurações de velocidade, potência e canais
  configurarRadio();
  ultimoTrafegoRede = millis(); // Marca o tempo de inicialização da rede

  // Mensagem inicial de apresentação na porta Serial
  if (MEU_ID == 0) {
    imprimirJsonGateway(); // O Gateway já começa mandando seu estado em JSON para o PC
  } else {
    Serial.print("BOOT ID=");
    Serial.print(MEU_ID);
    Serial.print(" CHIP_OK=");
    Serial.println(radio.isChipConnected() ? 1 : 0);
  }
}

// ============================================================================
// FUNÇÃO: loop() [Ciclo Infinito de Execução]
// ----------------------------------------------------------------------------
// O QUE FAZ: O "coração" do programa. O microcontrolador executa esta função em
// círculos, milhares de vezes por segundo, até ser desligado da alimentação.
// ESTRUTURA DO LOOP:
//   - Parte 1: Verifica se chegou alguma coisa pelo rádio e processa.
//   - Parte 2: Executa tarefas temporizadas específicas de cada papel (ID 0, 1 ou 2).
// ============================================================================
void loop() {
  // Vigilância constante: se o rádio sofrer oscilação de energia, religa-o imediatamente
  garantirRadioAtivo();

  // ==========================================================================
  // PARTE 1: CHEGOU ALGUMA MENSAGEM NO RÁDIO?
  // ==========================================================================
  if (radio.available()) {
    PacoteIoT pkt;
    radio.read(&pkt, sizeof(pkt)); // Retira a mensagem recebida e preenche a variável pkt
    ultimoTrafegoRede = millis();   // Ouvimos alguém: a rede está viva!

    // ------------------------------------------------------------------------
    // CASO A: Nós 1 e 2 recebendo o Farol (Beacon) emitido pelo Gateway
    // ------------------------------------------------------------------------
    if (pkt.tipo == MSG_BEACON && pkt.origem == 0 && MEU_ID != 0) {
      // Se o Gateway anunciou que a rede mudou de canal, muda junto agora mesmo!
      if (pkt.canal != canalAtual && pkt.canal <= 125) {
        aplicarCanal(pkt.canal);
      }

      // Guarda os valores antigos para saber se alguma coisa mudou
      uint8_t estadoAnterior = estadoAtuador;
      uint8_t modoAnterior = modoAuto;
      uint16_t limiarAnterior = limiar;

      // Atualiza os parâmetros de acordo com o que o Gateway ordenou
      modoAuto = pkt.modoAuto;
      limiar = pkt.limiar;

      if (!modoAuto) {
        // Modo Manual: o operador manda e a placa obedece cegamente
        estadoAtuador = pkt.estadoAtuador;
        atualizarSaidaFisica();
      } else if (MEU_ID == 2) {
        // Modo Automático no Nó 2: reavalia se a leitura atual é menor que o limiar
        estadoAtuador = (valorSensor < limiar) ? 1 : 0;
        atualizarSaidaFisica();
      }

      // Se alguma configuração mudou no Nó 2, ele avisa o Gateway mandando sua telemetria
      if (MEU_ID == 2 && (estadoAtuador != estadoAnterior || modoAuto != modoAnterior || limiar != limiarAnterior)) {
        delay(15);
        enviarTelemetriaParaGateway();
      }
    }

    // ------------------------------------------------------------------------
    // CASO B: O Nó Atuador (ID 2) recebe dados de sensoriamento M2M do Nó Sensor (ID 1)
    // ------------------------------------------------------------------------
    else if (MEU_ID == 2 && pkt.tipo == MSG_DADOS_M2M && pkt.destino == 2) {
      // 1. Devolve imediatamente um recibo (ACK) para o Nó 1 saber que a mensagem chegou
      PacoteIoT ack = pkt;
      ack.tipo = MSG_ACK;
      ack.origem = 2;
      ack.destino = pkt.origem;
      delay(2);          // Pequena pausa de 2ms para o transmissor do Nó 1 entrar em modo de escuta
      enviarRaw(ack);    // Dispara o recibo no ar!

      // 2. Só processa a leitura se o pacote não for duplicado
      if (pacoteNovo(pkt.seq)) {
        valorSensor = pkt.valorSensor; // Salva o valor analógico que veio do sensor

        // Se estivermos em modo automático, toma a decisão inteligente:
        // Exemplo: se o sensor ler escuridão (valor menor que o limiar), acende a lâmpada!
        if (modoAuto) {
          estadoAtuador = (valorSensor < limiar) ? 1 : 0;
          atualizarSaidaFisica(); // Atualiza o pino digital D4
        }

        Serial.println("[NO 2] RX do NO 1 -> enviando ao GW");
        delay(15);
        enviarTelemetriaParaGateway(); // Repassa tudo para o Gateway ficar atualizado!
      }
    }

    // ------------------------------------------------------------------------
    // CASO C: O Gateway (ID 0) recebe a telemetria do Nó Atuador (ID 2)
    // ------------------------------------------------------------------------
    else if (MEU_ID == 0 && pkt.tipo == MSG_TELEMETRIA && pkt.destino == 0) {
      // 1. Devolve um recibo (ACK) de volta ao Nó 2
      PacoteIoT ack = pkt;
      ack.tipo = MSG_ACK;
      ack.origem = 0;
      ack.destino = pkt.origem;
      delay(2);
      enviarRaw(ack);

      // 2. Se for um pacote inédito, atualiza as variáveis internas do Gateway
      if (pacoteNovo(pkt.seq)) {
        valorSensor = pkt.valorSensor;
        estadoAtuador = pkt.estadoAtuador;
        modoAuto = pkt.modoAuto;
        limiar = pkt.limiar;
        totalRpdHits = pkt.rpdHits;
        totalRetries = pkt.retries;
        pacotesRecebidos++;

        // Emite a linha em JSON pela porta USB para atualizar o site no navegador
        imprimirJsonGateway();
      }
    }
  }

  // ==========================================================================
  // PARTE 2: TAREFAS TEMPORIZADAS (O QUE CADA PLACA FAZ NO SEU TEMPO LIVRE)
  // ==========================================================================

  // SE FOR O GATEWAY (MEU_ID == 0):
  if (MEU_ID == 0) {
    // 1. Lê os comandos que o computador enviar pela porta USB
    lerComandosSerialGateway();

    // 2. Emite o sinal do "Farol" (Beacon) periodicamente:
    //    - A cada 80 milissegundos se estiver na janela rápida após trocar de canal
    //    - A cada 1500 milissegundos (1,5 segundos) em condições normais
    unsigned long intervaloBeacon = (millis() < janelaBeaconRapidoAte) ? 80 : 1500;
    if (millis() - ultimoEnvio >= intervaloBeacon) {
      ultimoEnvio = millis();
      dispararBeacon(canalAtual, 1); // Dispara 1 beacon no ar
    }
  }
  // SE FOREM OS NÓS REMOTOS (NÓ 1 SENSOR OU NÓ 2 ATUADOR):
  else {
    // 1. Detecção de isolamento / Varredura de resgate:
    // Se passar mais de 5000 milissegundos (5 segundos) sem ouvir nenhum tráfego da rede,
    // deduz que a rede mudou de frequência e faz o Auto-Scan por todos os canais!
    if (millis() - ultimoTrafegoRede > 5000) {
      buscarCanalGateway();
      ultimoTrafegoRede = millis();
    }

    // 2. Comportamento exclusivo do NÓ SENSOR (MEU_ID == 1):
    // A cada 1200 milissegundos (1,2 segundos), faz a leitura do sensor físico e envia ao Nó 2
    if (MEU_ID == 1 && (millis() - ultimoEnvio >= 1200)) {
      ultimoEnvio = millis();

      PacoteIoT m2m;
      m2m.tipo = MSG_DADOS_M2M;                 // Tipo de mensagem: Dados M2M
      m2m.destino = 2;                           // Destino direto: Nó 2 (Atuador)
      m2m.valorSensor = analogRead(PINO_SENSOR); // Lê o pino analógico A0 (0 a 1023)

      // Envia com garantia de entrega (esperando o recibo do Nó 2)
      bool ok = enviarConfiavel(m2m);
      Serial.println(ok ? "[NO 1] M2M -> NO 2 OK" : "[NO 1] FALHA SEM ACK DO NO 2");
    }
  }
}
