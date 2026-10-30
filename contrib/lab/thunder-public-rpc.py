#!/usr/bin/env python3
"""Read-only JSON-RPC front for a Thunder node, for a block explorer (the lab server's mainnet node).

    thunder-public-rpc.py <bind host> <bind port> <node RPC port> <credentials file>

Accepts one JSON-RPC request object per POST (no batches) with HTTP Basic credentials equal to the one line
`user:password` of the credentials file (read once at start), and forwards it to the node's public RPC on
127.0.0.1 only when its method is on the read-only list; anything else gets -32601. The firewall decides who may
connect; the credentials guard against anyone else the firewall lets in.
"""
import base64
import hmac
import http.client
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# thunder's public methods (rpc-api/lib.rs), minus the two that change state: connect_block, submit_transaction
READ_ONLY = frozenset({
    "getblockcount", "get_block_hash", "get_block", "get_bmm_inclusions", "get_best_mainchain_block_hash",
    "get_best_sidechain_block_hash", "get_transaction", "get_utxos", "get_stxos", "list_utxos", "list_peers",
    "sidechain_wealth", "pending_withdrawal_bundle", "get_withdrawal_bundle", "latest_failed_withdrawal_bundle_height",
    "openapi_schema",
})
MAX_BODY = 64 * 1024


class Handler(BaseHTTPRequestHandler):
    server_version = "thunder-public-rpc"
    timeout = 15
    upstream = ("127.0.0.1", 0)
    auth = b""

    def _reply(self, code, body, extra=None):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _authorized(self):
        h = self.headers.get("Authorization", "")
        if not h.startswith("Basic "):
            return False
        try:
            got = base64.b64decode(h[6:], validate=True)
        except ValueError:
            return False
        return hmac.compare_digest(got, self.auth)

    def do_POST(self):
        if not self._authorized():
            return self._reply(401, {"error": "unauthorized"}, {"WWW-Authenticate": 'Basic realm="thunder"'})
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            return self._reply(413, {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request"}})
        raw = self.rfile.read(length)
        try:
            req = json.loads(raw)
        except ValueError:
            return self._reply(200, {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}})
        if not isinstance(req, dict) or not isinstance(req.get("method"), str):
            return self._reply(200, {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request"}})
        if req["method"] not in READ_ONLY:
            return self._reply(200, {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32601, "message": "Method not found"}})
        body = json.dumps({"jsonrpc": "2.0", "id": req.get("id"), "method": req["method"], "params": req.get("params", [])}).encode()
        try:
            conn = http.client.HTTPConnection(*self.upstream, timeout=60)
            conn.request("POST", "/", body, {"Content-Type": "application/json"})
            resp = conn.getresponse()
            data = resp.read()
            conn.close()
        except OSError:
            return self._reply(502, {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32603, "message": "Node unavailable"}})
        self.send_response(resp.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._reply(405, {"error": "POST JSON-RPC only"})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    host, port, node_port, cred_file = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    with open(cred_file, "rb") as f:
        cred = f.read().strip()
    if b":" not in cred or len(cred) < 20:
        sys.exit("credentials file: one line user:password")
    Handler.upstream = ("127.0.0.1", node_port)
    Handler.auth = cred
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer((host, port), Handler).serve_forever()
