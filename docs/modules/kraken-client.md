---
aliases: [kraken client, kraken ws, ixwebsocket, nonce, kraken rest]
sources: [src/feed_handler/kraken/**]
decisions: [decisions/0001-exchange-feeds.md, decisions/0010-connection-thread-handoff.md, decisions/0011-desync-recovery.md]
---

# Kraken client

The Kraken `level3` WebSocket client (`kraken_ws_client.*`), the REST client that
fetches WebSocket tokens and `AssetPairs` (`kraken_rest_client.*`), and request
signing with its nonce (`kraken_signing.*`). Wire facts are in
`exchanges/kraken.md`; this is how the client behaves and why.

## Connection and reconnection

The transport is IXWebSocket, which owns the socket and its own thread
(`decisions/0010` on why that keeps Kraken out of an epoll group).

- **IXWebSocket's automatic reconnection is used, with explicit bounds.** It
  already does exponential backoff with jitter, but its default minimum wait is
  1 ms, which would retry the signed `GetWebSocketsToken` call far faster than
  Kraken's rate limits allow. Bounds are 1 s and 30 s.
- **The client also rate-limits connection setup itself**
  (`ThrottleConnectionSetup`). Confirmed live: the library's bounds apply only
  between *failed* attempts, so a connection that succeeds and is then torn down
  (by the watchdog or by Kraken) is re-established at once, each time costing a
  signed REST call. The client keeps the same floor between setups, and a
  failure after the socket connects (token fetch, subscribe send) backs off
  exponentially on top.
- **Forcing a reconnect is `close()`, never `stop()`.** `stop()` joins the
  library's thread and ends automatic reconnection for good; it is only right at
  process shutdown, where automatic reconnection is turned off first so the
  close is not taken as a forced reconnect.
- **Liveness is two mechanisms.** `setPingInterval` (off by default in the
  library, so set on purpose) and the application-level staleness watchdog
  (`decisions/0011`, `staleness_watchdog.h`). Ping/pong frames count as proof of
  life but are not journaled: they are IXWebSocket's protocol, not Kraken's.
- **The watchdog disarms itself when it fires** and rearms on the next
  connection's first inbound frame; otherwise it would stay stale through the
  reconnect and tear down each new connection before it delivered anything.
  Kraken needs a separate watchdog thread because IXWebSocket owns the receive
  loop and offers no tick to hook.

## Inbound messages

- **Journaled before they are classified.** Classification does the minimum
  needed to log a rejected subscribe (the connection stays open and just never
  delivers data, `exchanges/kraken.md`); nothing this build fails to understand
  may cost a message its journal record.
- **A failed journal write is fatal**, as it is for Deribit. A message that
  arrives before the first `connect_id` is not: it is recoverable on the next
  connect.
- The session is closed on IXWebSocket's `Close` event, so a lost socket ends
  its `connect_id` and closes its file. The `Error` event is only raised for
  failed connection attempts, so no socket loss skips `Close`.

## Nonce persistence

Kraken's nonce must strictly increase per API key, and one nonce below a value
already used rejects every later call until the clock catches up.
`PersistentNonceSource` (`kraken_signing.*`, file `state/kraken-nonce.state`)
covers a restart landing on a backwards clock step:

- **It wraps `NonceGenerator`**, which stays a pure `max(now, last + 1)` rule
  with an injectable clock, so bursts and backwards steps are unit-testable.
  Without a path the wrapper behaves exactly like the bare generator.
- **Neither file nor clock is trusted alone**: startup seeds the high-water
  mark, and the next nonce is `max(persisted + 1, now_micros)`.
- **Written on every issuance.** A signed call happens at most once per
  (re)connect, so batching buys nothing and a batched mark is the one that is
  stale after a crash.
- **Temp file and rename, no fsync.** The failure that matters is a smaller mark
  surviving, which a torn write can produce and a rename cannot. The clock
  covers a lost file.
- **Every file error is a warning, never a startup failure.** The opposite of
  the journal rule, for the opposite reason: a nonce file is a backstop for a
  case the clock almost always handles.
- Not solved: two processes sharing one API key. That needs a shared mark or one
  key each.

## Testing

There is no live-socket harness (IXWebSocket owns the thread and fd, and a real
connection needs a signed token call), so `WsClient::HandleMessage` is public and
driven directly, as `RestClient::ParseAssetPairs` is. A journal failure is
produced by latching the writer's sticky error with an oversized record, the
same state a full disk leaves.
