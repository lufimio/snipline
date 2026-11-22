"""Loopback-only HTTP fixture for native multipart uploads."""
import http.server
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        with open(sys.argv[2], "wb") as output:
            output.write(data)
        response = b'{"files":[{"url":"https://example.com/json-upload"}]}' if self.path == "/json" else b"https://example.com/fixture-upload"
        self.send_response(503 if self.path == "/error" else 200)
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *_):
        pass


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
with open(sys.argv[1], "w") as output:
    output.write(str(server.server_port))
server.serve_forever()
