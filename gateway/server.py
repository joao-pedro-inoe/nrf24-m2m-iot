# Trecho alterado na classe Handler (rota "/") dentro de gateway/server.py:
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
HTML_PATH = os.path.join(BASE_DIR, "static", "index.html")

# Dentro de do_GET:
if parsed.path == "/":
    with open(HTML_PATH, "rb") as f:
        conteudo = f.read()
    self.send_response(200)
    self.send_header("Content-Type", "text/html; charset=utf-8")
    self.end_headers()
    self.wfile.write(conteudo)
