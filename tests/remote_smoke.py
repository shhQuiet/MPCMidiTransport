"""Run the native VST smoke test against a local Open MPC Remote fixture."""

from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import subprocess
import sys
import threading


class Remote(BaseHTTPRequestHandler):
    buttons = ["STOP", "FULL-LEVEL", *[f"BUTTON-{n}" for n in range(22)], "MATRIX", "CLIP"]
    requests = []

    def do_GET(self):
        self.requests.append(self.path)
        if self.path == "/buttons":
            body = json.dumps(self.buttons).encode()
        elif self.path == "/button?name=MATRIX":
            body = b"ok"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    smoke_test, plugin = sys.argv[1:]
    with HTTPServer(("127.0.0.1", 6720), Remote) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([smoke_test, plugin, "--remote"], check=True)
        finally:
            server.shutdown()
            worker.join()
    assert Remote.requests.count("/buttons") >= 3, Remote.requests
    assert Remote.requests.count("/button?name=MATRIX") == 1, Remote.requests
    print("PASS device button discovery, pagination, and dispatch of a restored named mapping")
