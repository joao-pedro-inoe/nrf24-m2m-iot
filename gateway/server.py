import atexit
import glob
import json
import os
import signal
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, unquote, urlparse
import serial

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
HTML_PATH = os.path.join(BASE_DIR, "static", "index.html")

BAUD_RATE = 115200
PORTA_HTTP = int(os.environ.get("PORTA_HTTP", "8081"))

httpd = None
encerrando = False
lock = threading.Lock()

conexoes = {}       # porta -> serial.Serial
info_portas = {}    # porta -> dict(papel, status, erro_hw, ultimo_msg, rx_linhas)
porta_gateway = None

logs = []
log_seq = 0
ultimo_rx_rf_ts = 0.0
ultimo_eco_gw_ts = 0.0
tx_count = 0

diag_flags = {
    "hw_erro_portas": set(),
    "salto1_falha": False,
    "salto1_ok": False,
    "salto2_tx": False,
}

estado = {
    "canal": 100,
    "sensor": 0,
    "atuador": 0,
    "modoAuto": 1,
    "limiar": 500,
    "rpdHits": 0,
    "retries": 0,
    "rxOk": 0,
    "sync": "Aguardando..."
}

def registrar_log(tipo, porta, msg):
    global log_seq
    ts = time.strftime("%H:%M:%S")
    with lock:
        log_seq += 1
        logs.append({
            "id": log_seq,
            "ts": ts,
            "tipo": tipo,
            "porta": porta,
            "msg": msg
        })
        if len(logs) > 300:
            del logs[:len(logs) - 300]
    print(f"[{ts}] [{tipo}] [{porta}] {msg}", flush=True)

def encerrar_sistema(*args):
    global encerrando
    if encerrando:
        return
    encerrando = True
    print("\nLiberando portas seriais e HTTP...")
    for p, s in list(conexoes.items()):
        try:
            if s and s.is_open:
                s.close()
        except Exception:
            pass
    try:
        if httpd:
            httpd.server_close()
    except Exception:
        pass
    print("Portas liberadas com sucesso.")
    os._exit(0)

signal.signal(signal.SIGINT, encerrar_sistema)
signal.signal(signal.SIGTERM, encerrar_sistema)
atexit.register(encerrar_sistema)

def obter_ip_local():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

def resetar_placas_dtr(alvo="ALL"):
    with lock:
        diag_flags["hw_erro_portas"].clear()
        diag_flags["salto1_falha"] = False
        diag_flags["salto1_ok"] = False
        diag_flags["salto2_tx"] = False
        alvos = list(conexoes.items()) if alvo == "ALL" else [(alvo, conexoes.get(alvo))]

    for p, s in alvos:
        if not s or not s.is_open:
            continue
        try:
            s.dtr = False
            time.sleep(0.08)
            s.dtr = True
            registrar_log("SYS", p, "Pulso DTR enviado (placa reiniciada para leitura de boot)")
        except Exception as e:
            registrar_log("ERR", p, f"Falha ao reiniciar via DTR: {e}")

def gerar_diagnostico():
    with lock:
        portas_ordenadas = sorted(info_portas.keys())
        hw_erros = sorted(diag_flags["hw_erro_portas"])
        rf_ativo = (time.time() - ultimo_rx_rf_ts) < 4.5 and estado["rxOk"] > 0
        gw_ativo = (time.time() - ultimo_eco_gw_ts) < 15.0 or estado["rxOk"] > 0
        rx_ok = estado["rxOk"]
        rpd = estado["rpdHits"]
        ret = estado["retries"]
        ch = estado["canal"]
        s1_falha = diag_flags["salto1_falha"]
        s2_tx = diag_flags["salto2_tx"]
        gw = porta_gateway

    if hw_erros:
        return {
            "nivel": "erro",
            "titulo": f"FALHA DE HARDWARE SPI/3.3V EM: {', '.join(hw_erros)}",
            "detalhe": "O Arduino iniciou, mas o módulo nRF24L01 não respondeu (radio.begin() falhou). Verifique jumpers CE=7, CSN=8, SCK=13, MOSI=11, MISO=12 e alimentação 3.3V/GND nessa placa."
        }

    if len(portas_ordenadas) < 3:
        return {
            "nivel": "aviso",
            "titulo": f"APENAS {len(portas_ordenadas)} PLACA(S) DETECTADA(S) NO USB",
            "detalhe": f"Portas ativas: {', '.join(portas_ordenadas) or 'nenhuma'}. Verifique se os 3 Arduinos estão alimentados."
        }

    if not gw_ativo:
        return {
            "nivel": "aviso",
            "titulo": f"GATEWAY ({gw}) SEM ECO SERIAL RECENTE",
            "detalhe": "Clique em 'Resetar Placas (DTR)' para forçar o Gateway a enviar seu JSON de inicialização e confirmar se MEU_ID=0 está gravado nele."
        }

    if rf_ativo:
        if ret > 15 or rpd > 10:
            return {
                "nivel": "aviso",
                "titulo": f"ENLACE RF ATIVO COM INTERFERÊNCIA NO CANAL {ch}",
                "detalhe": f"Pacotes entregues ({rx_ok}), mas com RPD={rpd} e ARQ Retries={ret}. O CSMA/CA está compensando ruído ativo na faixa de 2.4 GHz."
            }
        return {
            "nivel": "ok",
            "titulo": "REDE 100% OPERACIONAL (NÓ 1 -> NÓ 2 -> GATEWAY)",
            "detalhe": f"Telemetria fluindo normalmente no Canal {ch} ({rx_ok} pacotes confirmados com ACK)."
        }

    if s1_falha:
        return {
            "nivel": "erro",
            "titulo": "QUEBRA NO SALTO 1: NÓ 1 NÃO RECEBE ACK DO NÓ 2",
            "detalhe": "O Nó 1 está transmitindo, mas o Nó 2 não respondeu. Confirme se o Nó 2 está gravado com MEU_ID=2, no mesmo canal e sem erro de nRF24."
        }

    if s2_tx and rx_ok == 0:
        return {
            "nivel": "erro",
            "titulo": "QUEBRA NO SALTO 2: NÓ 2 RECEBE DO NÓ 1, MAS NÃO ALCANÇA O GATEWAY",
            "detalhe": f"O enlace M2M (Nó 1 -> Nó 2) funciona, mas o Gateway ({gw}) não recebeu a telemetria. Verifique se o Gateway está gravado com MEU_ID=0."
        }

    return {
        "nivel": "aviso",
        "titulo": f"GATEWAY OK ({gw}), AGUARDANDO TRÁFEGO DE USB1/USB2",
        "detalhe": f"Nenhum pacote RF chegou ainda no Canal {ch}. Clique em 'Resetar Placas (DTR)' para ler o boot das placas remotas."
    }

def monitorar_porta(porta, ser):
    global porta_gateway, ultimo_rx_rf_ts, ultimo_eco_gw_ts
    rx_anterior = -1

    while not encerrando:
        try:
            linha = ser.readline().decode("utf-8", errors="ignore").strip()
            if not linha:
                continue

            with lock:
                info_portas[porta]["rx_linhas"] += 1
                info_portas[porta]["ultimo_msg"] = time.strftime("%H:%M:%S")

            if linha.startswith("{") and linha.endswith("}"):
                dados = json.loads(linha)
                if "erro" in dados:
                    with lock:
                        info_portas[porta]["status"] = "ERRO nRF24"
                        info_portas[porta]["erro_hw"] = True
                        diag_flags["hw_erro_portas"].add(porta)
                    registrar_log("ERR", porta, f"BOOT FALHOU: {dados['erro']}")
                else:
                    with lock:
                        if porta_gateway != porta:
                            porta_gateway = porta
                        info_portas[porta]["papel"] = "GATEWAY (ID 0)"
                        info_portas[porta]["status"] = "OK"
                        estado.update(dados)
                        estado["sync"] = time.strftime("%H:%M:%S")
                        ultimo_eco_gw_ts = time.time()

                    rx_atual = dados.get("rxOk", 0)
                    if rx_atual > rx_anterior and rx_atual > 0:
                        ultimo_rx_rf_ts = time.time()
                        registrar_log(
                            "RF", porta,
                            f"TELEMETRIA #{rx_atual} (Nó 1 -> Nó 2 -> GW) | Sensor={dados.get('sensor')} | "
                            f"Atuador={dados.get('atuador')} | CH={dados.get('canal')} | "
                            f"RPD={dados.get('rpdHits')} | Retries={dados.get('retries')}"
                        )
                    else:
                        registrar_log(
                            "RX", porta,
                            f"ECO GATEWAY | Sensor={dados.get('sensor')} | Atuador={dados.get('atuador')} | "
                            f"Modo={'AUTO' if dados.get('modoAuto') else 'MANUAL'} | CH={dados.get('canal')}"
                        )
                    rx_anterior = rx_atual
            else:
                with lock:
                    if "BOOT ID=1" in linha:
                        info_portas[porta]["papel"] = "NÓ 1 (SENSOR)"
                    elif "BOOT ID=2" in linha:
                        info_portas[porta]["papel"] = "NÓ 2 (ATUADOR)"
                    if "CHIP_OK=0" in linha:
                        info_portas[porta]["status"] = "ERRO CHIP"
                        diag_flags["hw_erro_portas"].add(porta)
                    if "FALHA SEM ACK DO NO 2" in linha:
                        diag_flags["salto1_falha"] = True
                        diag_flags["salto1_ok"] = False
                    if "M2M -> NO 2 OK" in linha:
                        diag_flags["salto1_falha"] = False
                        diag_flags["salto1_ok"] = True
                    if "RX do NO 1" in linha:
                        diag_flags["salto2_tx"] = True

                tipo_msg = "ERR" if ("FALHA" in linha or "CHIP_OK=0" in linha) else "NODE"
                registrar_log(tipo_msg, porta, linha)

        except json.JSONDecodeError:
            registrar_log("RAW", porta, linha)
        except Exception as e:
            if not encerrando:
                with lock:
                    info_portas[porta]["status"] = "DESCONECTADO"
                registrar_log("ERR", porta, f"Erro de leitura serial: {e}")
                time.sleep(0.5)

portas_detectadas = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))

if not portas_detectadas:
    print("ERRO: Nenhuma placa encontrada em /dev/ttyUSB* ou /dev/ttyACM*. Verifique os cabos USB.")
    sys.exit(1)

porta_gateway = portas_detectadas[0]

for idx, p in enumerate(portas_detectadas):
    try:
        s = serial.Serial(p, BAUD_RATE, timeout=1)
        conexoes[p] = s
        papel_inicial = "GATEWAY (provável)" if idx == 0 else f"NÓ REMOTO #{idx}"
        info_portas[p] = {
            "papel": papel_inicial,
            "status": "ESCUTANDO",
            "erro_hw": False,
            "ultimo_msg": "-",
            "rx_linhas": 0
        }
        threading.Thread(target=monitorar_porta, args=(p, s), daemon=True).start()
        registrar_log("SYS", p, f"Porta aberta ({papel_inicial})")
    except Exception as e:
        registrar_log("ERR", p, f"Não foi possível abrir porta: {e}")

class Server(HTTPServer):
    allow_reuse_address = True

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        global tx_count
        parsed = urlparse(self.path)

        if parsed.path == "/":
            try:
                with open(HTML_PATH, "rb") as f:
                    conteudo = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(conteudo)
            except FileNotFoundError:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(b"Erro: gateway/static/index.html nao encontrado.")
                
        elif parsed.path == "/logo.png":
            logo_path = os.path.join(BASE_DIR, "static", "logo.png")
            try:
                with open(logo_path, "rb") as f:
                    img = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.end_headers()
                self.wfile.write(img)
            except FileNotFoundError:
                self.send_response(404)
                self.end_headers()
        
        elif parsed.path == "/reset":
            resetar_placas_dtr("ALL")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"OK")

        elif parsed.path == "/status":
            qs = parse_qs(parsed.query)
            since = int(qs.get("since", ["0"])[0])
            diag = gerar_diagnostico()
            with lock:
                novos_logs = [l for l in logs if l["id"] > since]
                rf_ativo = (time.time() - ultimo_rx_rf_ts) < 4.5 and estado["rxOk"] > 0
                payload = {
                    "estado": dict(estado),
                    "logs": novos_logs,
                    "gateway": porta_gateway,
                    "portas": {k: dict(v) for k, v in info_portas.items()},
                    "rfAtivo": rf_ativo,
                    "txCount": tx_count,
                    "diag": diag
                }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(payload).encode("utf-8"))

        

        elif parsed.path == "/cmd":
            qs = parse_qs(parsed.query)
            c = unquote(qs.get("c", [""])[0]).strip()
            p_alvo = unquote(qs.get("p", [""])[0]).strip() or porta_gateway
            if c and p_alvo in conexoes:
                with lock:
                    tx_count += 1
                    conexoes[p_alvo].write((c + "\n").encode("utf-8"))
                registrar_log("TX", p_alvo, f"Comando enviado: {c}")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"OK")

if __name__ == "__main__":
    ip_local = obter_ip_local()
    print(f"Interface ativa em: http://localhost:{PORTA_HTTP} ou http://{ip_local}:{PORTA_HTTP}")
    print("Pressione Ctrl+C para encerrar e liberar todas as portas.")
    httpd = Server(("0.0.0.0", PORTA_HTTP), Handler)
    httpd.serve_forever()
