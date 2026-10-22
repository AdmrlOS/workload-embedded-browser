"""Optional static app server. It has no signage, upload or scheduling APIs."""
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/healthz":
            body = b'{"status":"ok"}\n'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().do_GET()

    def list_directory(self, path):
        self.send_error(403, "Directory listing disabled")


if __name__ == "__main__":
    root = os.environ["BROWSER_HTTP_ROOT"]
    host = os.environ.get("BROWSER_HTTP_HOST", "127.0.0.1")
    port = int(os.environ.get("BROWSER_HTTP_PORT", "8080"))
    server = ThreadingHTTPServer((host, port), partial(Handler, directory=root))
    print(f"Serving embedded browser app on {host}:{port}", flush=True)
    server.serve_forever()
