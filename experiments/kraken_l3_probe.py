#!/usr/bin/env python3
"""Throwaway probe: get a Kraken WS auth token via REST, then subscribe to
the level3 (order-by-order) channel over WebSocket v2 and print raw messages.

NOT the real design -- just to see the actual wire format. See decisions/0001.

Usage:
    ./.venv/bin/python kraken_l3_probe.py
"""
import asyncio
import base64
import hashlib
import hmac
import json
import os
import time
import urllib.parse
import urllib.request
from pathlib import Path

import websockets

API_URL = "https://api.kraken.com"
WS_URL = "wss://ws-l3.kraken.com/v2"
SYMBOL = "BTC/USD"


def load_dotenv(path: Path) -> None:
    """Minimal .env loader -- avoids bash `source` mangling values that
    contain shell-special characters."""
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip())


load_dotenv(Path(__file__).parent / ".env")

API_KEY = os.environ["KRAKEN_API_KEY"]
API_SECRET = os.environ["KRAKEN_API_SECRET"]


def kraken_sign(urlpath: str, data: dict, secret: str) -> str:
    postdata = urllib.parse.urlencode(data)
    encoded = (str(data["nonce"]) + postdata).encode()
    message = urlpath.encode() + hashlib.sha256(encoded).digest()
    mac = hmac.new(base64.b64decode(secret), message, hashlib.sha512)
    return base64.b64encode(mac.digest()).decode()


def get_ws_token() -> str:
    urlpath = "/0/private/GetWebSocketsToken"
    data = {"nonce": str(int(time.time() * 1000))}
    headers = {
        "API-Key": API_KEY,
        "API-Sign": kraken_sign(urlpath, data, API_SECRET),
        "Content-Type": "application/x-www-form-urlencoded",
    }
    req = urllib.request.Request(
        API_URL + urlpath,
        data=urllib.parse.urlencode(data).encode(),
        headers=headers,
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = json.loads(resp.read())
    if body.get("error"):
        raise RuntimeError(f"Kraken REST error: {body['error']}")
    return body["result"]["token"]


async def main() -> None:
    token = get_ws_token()
    print(">>> got WS token")

    async with websockets.connect(WS_URL) as ws:
        subscribe = {
            "method": "subscribe",
            "params": {
                "channel": "level3",
                "symbol": [SYMBOL],
                "snapshot": True,
                "token": token,
            },
        }
        await ws.send(json.dumps(subscribe))
        print(">>> sent level3 subscribe for", SYMBOL)

        async for message in ws:
            print("<<<", message)


if __name__ == "__main__":
    asyncio.run(main())
