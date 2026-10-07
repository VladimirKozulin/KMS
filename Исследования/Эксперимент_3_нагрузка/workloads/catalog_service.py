"""Synthetic Python "microservice" for the KSM experiment — same memory model as CatalogService.java.

shared catalog (fixed seed, identical in every replica) + private sessions (random) + request churn.
Env: CATALOG_MB, PRIVATE_MB, PORT
"""
import json
import os
import random
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CATALOG_MB = int(os.environ.get("CATALOG_MB", "200"))
PRIVATE_MB = int(os.environ.get("PRIVATE_MB", "50"))
PORT = int(os.environ.get("PORT", "8080"))

shared = random.Random(42)
letters = "abcdefghijklmnopqrstuvwxyz"


def text(rng, n):
    return "".join(rng.choice(letters) for _ in range(n))


# ~1 KB of Python objects per product
catalog = [
    {"id": i, "sku": f"SKU-{i}", "title": text(shared, 48), "description": text(shared, 400),
     "price": shared.randint(0, 100000) / 100, "tags": [shared.randint(0, 999) for _ in range(8)]}
    for i in range(CATALOG_MB * 1024)
]

sessions = [bytearray(os.urandom(64 * 1024)) for _ in range(PRIVATE_MB * 16)]
print(f"ready: catalog={len(catalog)} sessions={len(sessions)}", flush=True)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        r = random.Random()
        start = r.randrange(len(catalog) - 200)
        window = catalog[start:start + 200]
        payload = {"sum": sum(p["price"] for p in window), "items": [p["sku"] for p in window[:20]]}
        s = sessions[r.randrange(len(sessions))]
        for _ in range(64):
            s[r.randrange(len(s))] = r.randrange(256)
        garbage = [bytes(1024) for _ in range(50)]  # noqa: F841 — short-lived churn
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
