#!/usr/bin/env python3
"""Capture a real Deribit WS `book` (L2) channel session to a JSON fixture
used by the golden L2 book's replay test
(src/order_book/tests/deribit_capture_replay_test.cpp). The checked-in copy is
packed into src/order_book/tests/data/capture_fixtures.tar.xz; see
docs/modules/order-book.md for how to replace it.

No probe for this channel existed yet -- deribit_fix_probe.py only covers
FIX, which is also L2 but a different wire shape (see decisions/0001).

ADR 0001 describes Deribit's `book` channel as public/no-auth -- that
turned out to be true only for the grouped/interval channels. The `raw`
channel (the one carrying change_id/prev_change_id, which is what this
capture is actually after) requires being logged in
(`raw_subscriptions_not_available_for_unauthorized`, confirmed against
the live testnet API). So this authenticates via public/auth with the
same DERIBIT_TESTNET_CLIENT_ID/SECRET deribit_fix_probe.py already uses,
rather than subscribing unauthenticated.

NOT the real design -- a one-off capture tool, like the other
experiments/ probes. Every message received is recorded as-is, with a
receipt timestamp; nothing is parsed or classified here.

Usage:
    ./.venv/bin/python deribit_ws_book_probe.py [--seconds N] [--output PATH]
"""
import argparse
import asyncio
import json
import os
import sys
import time
from pathlib import Path

import websockets

WS_URL = "wss://test.deribit.com/ws/api/v2"
INSTRUMENT = "BTC-PERPETUAL"
CHANNEL = f"book.{INSTRUMENT}.raw"
DEFAULT_OUTPUT = Path("deribit_book_capture.json")
DEFAULT_SECONDS = 120


def load_dotenv(path: Path) -> None:
    """Minimal .env loader -- avoids bash `source` mangling values that
    contain shell-special characters (quotes, $, #, etc. in a real secret)."""
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip())


load_dotenv(Path(__file__).parent / ".env")

CLIENT_ID = os.environ["DERIBIT_TESTNET_CLIENT_ID"]
CLIENT_SECRET = os.environ["DERIBIT_TESTNET_CLIENT_SECRET"]


async def capture(output_path: Path, seconds: int) -> None:
    journal = []
    start = time.monotonic()

    async with websockets.connect(WS_URL) as ws:
        # Deliberately NOT appended to journal: it carries the plaintext
        # client_secret, and the reply carries a live access/refresh
        # token. Neither belongs in a file meant to be inspected freely
        # and committed to the repo.
        auth = {
            "jsonrpc": "2.0",
            "id": 0,
            "method": "public/auth",
            "params": {
                "grant_type": "client_credentials",
                "client_id": CLIENT_ID,
                "client_secret": CLIENT_SECRET,
            },
        }
        await ws.send(json.dumps(auth))
        auth_reply = json.loads(await ws.recv())
        if "error" in auth_reply:
            raise RuntimeError(f"Deribit auth failed: {auth_reply['error']}")
        print(">>> authenticated", file=sys.stderr)

        subscribe = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "public/subscribe",
            "params": {"channels": [CHANNEL]},
        }
        journal.append({"direction": "sent", "t": time.monotonic() - start, "message": subscribe})
        await ws.send(json.dumps(subscribe))
        print(">>> sent public/subscribe for", CHANNEL, file=sys.stderr)

        deadline = start + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
            except asyncio.TimeoutError:
                break
            entry = {
                "direction": "received",
                "t": time.monotonic() - start,
                "message": json.loads(raw),
            }
            journal.append(entry)
            if len(journal) % 50 == 0:
                print(f">>> {len(journal)} journal entries so far", file=sys.stderr)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(
            {
                "exchange": "deribit",
                "channel": CHANNEL,
                "instrument": INSTRUMENT,
                "captured_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "capture_seconds": seconds,
                "journal": journal,
            },
            indent=2,
        )
    )
    print(f">>> wrote {len(journal)} journal entries to {output_path}", file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    asyncio.run(capture(args.output, args.seconds))


if __name__ == "__main__":
    main()
