import argparse
import glob
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH_DIR = os.path.join(ROOT_DIR, "firmware", "nrf24_node")
SERVER_SCRIPT = os.path.join(ROOT_DIR, "gateway", "server.py")


def listar_portas():
    portas = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if not portas:
        print("Nenhuma placa encontrada em /dev/ttyUSB* ou /dev/ttyACM*.")
    else:
        print("Portas detectadas:")
        for p in portas:
            print(f"  - {p}")
    return portas


def gravar_no(node_id, porta, fqbn):
    print(f"\n==> Compilando e gravando MEU_ID={node_id} na porta {porta}...")
    build_dir = os.path.join("/tmp", f"nrf24_build_id_{node_id}")
    cmd_compile = [
        "arduino-cli", "compile",
        "--fqbn", fqbn,
        "--build-path", build_dir,
        "--build-property", f"compiler.cpp.extra_flags=-DMEU_ID={node_id}",
        SKETCH_DIR,
    ]
    subprocess.run(cmd_compile, check=True)

    cmd_upload = [
        "arduino-cli", "upload",
        "-p", porta,
        "--fqbn", fqbn,
        "--input-dir", build_dir,
    ]
    subprocess.run(cmd_upload, check=True)
    print(f"[OK] Nó {node_id} gravado com sucesso em {porta}!")


def main():
    parser = argparse.ArgumentParser(
        prog="nrf-iot",
        description="CLI para gravação e monitoramento da rede IoT nRF24L01"
    )
    sub = parser.add_subparsers(dest="comando", required=True)

    # Subcomando: scan
    sub.add_parser("scan", help="Lista portas USB conectadas")

    # Subcomando: flash (um nó específico)
    p_flash = sub.add_parser("flash", help="Compila e grava um nó específico")
    p_flash.add_argument("--id", type=int, choices=[0, 1, 2], required=True, help="0=Gateway, 1=Sensor, 2=Atuador")
    p_flash.add_argument("--port", required=True, help="Ex: /dev/ttyUSB0")
    p_flash.add_argument("--fqbn", default="arduino:avr:nano:cpu=atmega328old", help="Placa alvo (padrão: Nano old bootloader)")

    # Subcomando: flash-all (grava os 3 nós de uma vez)
    p_all = sub.add_parser("flash-all", help="Grava IDs 0, 1 e 2 automaticamente nas 3 primeiras portas USB")
    p_all.add_argument("--fqbn", default="arduino:avr:nano:cpu=atmega328old", help="Placa alvo")

    # Subcomando: run (sobe o servidor web)
    p_run = sub.add_parser("run", help="Inicia o gateway web de monitoramento")
    p_run.add_argument("--port", type=int, default=8081, help="Porta HTTP (padrão: 8081)")

    args = parser.parse_args()

    if args.comando == "scan":
        listar_portas()

    elif args.comando == "flash":
        gravar_no(args.id, args.port, args.fqbn)

    elif args.comando == "flash-all":
        portas = listar_portas()
        if len(portas) < 3:
            print("Erro: São necessárias 3 placas conectadas para usar 'flash-all'.")
            sys.exit(1)
        for node_id, porta in enumerate(portas[:3]):
            gravar_no(node_id, porta, args.fqbn)

    elif args.comando == "run":
        os.environ["PORTA_HTTP"] = str(args.port)
        subprocess.run([sys.executable, SERVER_SCRIPT])


if __name__ == "__main__":
    main()