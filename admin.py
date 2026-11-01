import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Optional

PAGE = Path(__file__).with_name("admin.html")


def make_admin_server(
    host: str,
    port: int,
    get_status: Callable[[], Dict[str, Any]],
    run_command: Callable[[str], Optional[str]],
) -> ThreadingHTTPServer:
    """The admin page: shows the game's status and sends start/stop/mode commands."""

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path == "/":
                self._send(200, "text/html; charset=utf-8", PAGE.read_bytes())
            elif self.path == "/status":
                self._send_json(200, get_status())
            else:
                self._send_json(404, {"error": "Not found"})

        def do_POST(self) -> None:
            # Requiring JSON stops other sites open in the browser from sending
            # commands, since browsers won't send it cross-site without asking
            if self.path != "/command":
                self._send_json(404, {"error": "Not found"})
                return
            if self.headers.get("Content-Type") != "application/json":
                self._send_json(415, {"error": "Expected JSON"})
                return
            try:
                length = int(self.headers.get("Content-Length", 0))
                command = json.loads(self.rfile.read(length))["command"]
            except (ValueError, KeyError, TypeError):
                self._send_json(400, {"error": 'Expected {"command": ...}'})
                return
            error = run_command(str(command))
            self._send_json(200 if error is None else 409, {"error": error})

        def _send_json(self, status: int, body: Dict[str, Any]) -> None:
            self._send(status, "application/json", json.dumps(body).encode())

        def _send(self, status: int, content_type: str, body: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: Any) -> None:
            # The page asks for the status twice a second, so don't log requests
            pass

    return ThreadingHTTPServer((host, port), Handler)
