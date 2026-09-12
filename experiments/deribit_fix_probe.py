#!/usr/bin/env python3
"""Throwaway probe: log on to Deribit's FIX testnet, request BTC-PERPETUAL
market data, and print raw incoming FIX messages.

NOT the real design -- this is purely to see the actual wire format before
building anything real in C++. See decisions/0001 and 0002.

Usage:
    source .env  # or export the vars another way
    ./.venv/bin/python deribit_fix_probe.py
"""
import base64
import hashlib
import os
import secrets
import socket
import time
from pathlib import Path

import simplefix

HOST = "fix-test.deribit.com"
PORT = 9881


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


def build_logon(seq_num: int) -> simplefix.FixMessage:
    timestamp_ms = str(int(time.time() * 1000)).encode()
    nonce = base64.b64encode(secrets.token_bytes(32))
    raw_data = timestamp_ms + b"." + nonce
    password = base64.b64encode(
        hashlib.sha256(raw_data + CLIENT_SECRET.encode()).digest()
    )

    msg = simplefix.FixMessage()
    msg.append_pair(8, "FIX.4.4", header=True)
    msg.append_pair(35, "A", header=True)
    msg.append_pair(49, CLIENT_ID, header=True)  # SenderCompID
    msg.append_pair(56, "DERIBITSERVER", header=True)  # TargetCompID
    msg.append_pair(34, seq_num, header=True)  # MsgSeqNum
    msg.append_utc_timestamp(52, header=True)  # SendingTime
    msg.append_pair(98, 0)  # EncryptMethod: none
    msg.append_pair(108, 30)  # HeartBtInt
    msg.append_pair(553, CLIENT_ID)  # Username
    msg.append_pair(96, raw_data)  # RawData
    msg.append_pair(554, password)  # Password
    return msg


def build_market_data_request(seq_num: int, symbol: str) -> simplefix.FixMessage:
    msg = simplefix.FixMessage()
    msg.append_pair(8, "FIX.4.4", header=True)
    msg.append_pair(35, "V", header=True)  # MarketDataRequest
    msg.append_pair(49, CLIENT_ID, header=True)
    msg.append_pair(56, "DERIBITSERVER", header=True)
    msg.append_pair(34, seq_num, header=True)
    msg.append_utc_timestamp(52, header=True)
    msg.append_pair(262, "probe-1")  # MDReqID
    msg.append_pair(263, 1)  # SubscriptionRequestType: snapshot + updates
    msg.append_pair(264, 0)  # MarketDepth: 0 = full book
    msg.append_pair(267, 2)  # NoMDEntryTypes
    msg.append_pair(269, 0)  # MDEntryType: Bid
    msg.append_pair(269, 1)  # MDEntryType: Offer
    msg.append_pair(146, 1)  # NoRelatedSym
    msg.append_pair(55, symbol)  # Symbol
    return msg


def main() -> None:
    sock = socket.create_connection((HOST, PORT), timeout=10)
    parser = simplefix.FixParser()
    seq_num = 1

    logon = build_logon(seq_num)
    sock.sendall(logon.encode())
    print(">>> sent Logon")
    seq_num += 1

    subscribed = False

    while True:
        data = sock.recv(4096)
        if not data:
            print("connection closed")
            break
        parser.append_buffer(data)
        while True:
            msg = parser.get_message()
            if msg is None:
                break
            print("<<<", msg)

            msg_type = msg.get(35)
            if msg_type == b"A" and not subscribed:
                # Logon accepted -- subscribe to BTC-PERPETUAL market data.
                req = build_market_data_request(seq_num, "BTC-PERPETUAL")
                sock.sendall(req.encode())
                print(">>> sent MarketDataRequest for BTC-PERPETUAL")
                seq_num += 1
                subscribed = True


if __name__ == "__main__":
    main()
