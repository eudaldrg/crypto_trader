#!/usr/bin/env python3
"""Throwaway probe: Bitso's public `diff-orders` WebSocket channel plus the
unaggregated REST order book, to check it is genuine L3 and live from here.

NOT the real design -- just to see the actual wire format. See
docs/investigations/2026-09-23-free-l3-feeds.md.

Public data, no credentials. Prints only counts and key names, plus a few
sample entries (public market data).

Checks:
  - the subscribe is accepted and diff-orders arrive;
  - `sequence` increments by one per message (gaps counted);
  - the REST snapshot (`aggregate=false`) carries per-order `oid`s and a
    `sequence` that can be spliced with the buffered stream;
  - which `t` value means buy: order ids seen in both the stream and the
    snapshot's bids or asks settle it empirically.

Usage:
    ./.venv/bin/python bitso_l3_probe.py [book] [seconds]
    (default btc_mxn, 60 s)
"""
import asyncio
import collections
import json
import sys
import time
import urllib.request

import websockets

WS_URL = "wss://ws.bitso.com"
REST_URL = "https://api.bitso.com/api/v3/order_book/?book={book}&aggregate=false"


def fetch_snapshot(book: str) -> dict:
    req = urllib.request.Request(REST_URL.format(book=book),
                                 headers={"User-Agent": "crypto_trader-probe"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read())


async def main(book: str, seconds: float) -> None:
    messages = []  # diff-orders messages, in arrival order
    other_types = collections.Counter()
    first_frames = []
    snapshot = None
    snapshot_at = None

    async with websockets.connect(WS_URL, max_size=None) as ws:
        await ws.send(json.dumps({"action": "subscribe", "book": book, "type": "diff-orders"}))
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - time.monotonic()))
            except asyncio.TimeoutError:
                break
            msg = json.loads(raw)
            if len(first_frames) < 3:
                first_frames.append(msg)
            if msg.get("type") == "diff-orders" and "payload" in msg:
                messages.append(msg)
                # Take the REST snapshot after the stream has started, as the
                # documented resync procedure does, so the two can be spliced.
                if snapshot is None and len(messages) >= 5:
                    snapshot = await asyncio.to_thread(fetch_snapshot, book)
                    snapshot_at = len(messages)
            else:
                other_types[str(msg.get("type"))] += 1

    print(f"book={book} seconds={seconds}")
    print("first frames (types/keys):",
          [(f.get("type"), f.get("action"), f.get("response"), sorted(f.keys())) for f in first_frames])
    print("non-diff frame types:", dict(other_types))
    print("diff-orders messages:", len(messages))
    if not messages:
        return

    seqs = [int(m["sequence"]) for m in messages]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    print(f"sequence first={seqs[0]} last={seqs[-1]} gaps={gaps} "
          f"non-increasing={sum(1 for a, b in zip(seqs, seqs[1:]) if b <= a)}")
    print("message keys:", sorted(messages[0].keys()))

    entries = [e for m in messages for e in m["payload"]]
    per_msg = collections.Counter(len(m["payload"]) for m in messages)
    print("entries:", len(entries), "entries-per-message histogram:", dict(sorted(per_msg.items())))
    print("entry key sets:", collections.Counter(tuple(sorted(e.keys())) for e in entries))
    print("status s:", collections.Counter(e.get("s") for e in entries))
    print("side t:", collections.Counter(e.get("t") for e in entries))
    zero_amount = collections.Counter((e.get("s"), e.get("a") in (None, "0", "0.0") or
                                       (e.get("a") and float(e["a"]) == 0)) for e in entries)
    print("(status, amount-is-zero-or-missing):", dict(zero_amount))
    oid_count = collections.Counter(e.get("o") for e in entries)
    print("distinct oids:", len(oid_count), "oids seen more than once:",
          sum(1 for c in oid_count.values() if c > 1))
    print("sample entries:", entries[:3])

    if snapshot is None:
        print("no snapshot taken (fewer than 5 messages)")
        return
    payload = snapshot.get("payload", {})
    bids, asks = payload.get("bids", []), payload.get("asks", [])
    snap_seq = int(payload.get("sequence", -1))
    print(f"snapshot: success={snapshot.get('success')} keys={sorted(payload.keys())} "
          f"bids={len(bids)} asks={len(asks)} sequence={snap_seq} "
          f"(taken after message #{snapshot_at}, whose sequence is {seqs[snapshot_at - 1]})")
    if bids:
        print("snapshot bid keys:", sorted(bids[0].keys()), "sample:", bids[0])
    covered = [s for s in seqs if s > snap_seq]
    print(f"stream messages after the snapshot's sequence: {len(covered)}; "
          f"buffered stream starts at {seqs[0]} (<= snapshot+1 means it can be spliced: "
          f"{seqs[0] <= snap_seq + 1})")

    bid_oids = {b.get("oid") for b in bids}
    ask_oids = {a.get("oid") for a in asks}
    side_votes = collections.Counter()
    for e in entries:
        if e.get("o") in bid_oids:
            side_votes[(e.get("t"), "bid")] += 1
        elif e.get("o") in ask_oids:
            side_votes[(e.get("t"), "ask")] += 1
    print("side check, (t, side in snapshot):", dict(side_votes))


if __name__ == "__main__":
    book_arg = sys.argv[1] if len(sys.argv) > 1 else "btc_mxn"
    seconds_arg = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    asyncio.run(main(book_arg, seconds_arg))
