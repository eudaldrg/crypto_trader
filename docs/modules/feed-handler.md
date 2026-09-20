---
aliases: [feed handler, feed-handler, capture, config, connections]
sources: [src/feed_handler/**, config/**]
decisions: [decisions/0001-feed-source-selection.md, decisions/0004-feed-handler-architecture.md]
---

# Feed handler

The capture side of the pipeline: `kraken_feed_handler` and
`deribit_feed_handler` connect to an exchange, subscribe to the configured
symbols, and journal every inbound wire message verbatim. Which connections to
open is a TOML file passed with `--config`; the design rationale is in
`decisions/0004`, and this is what the file means and what to watch for.

## One connection, one socket, one journal

Each `[[connections]]` entry is one socket, one subscribe and one journal file
sequence. A binary opens every entry for its own exchange and ignores the rest,
so one config file can drive both:

```
kraken_feed_handler  --config config/feed_handler.toml
deribit_feed_handler --config config/feed_handler.toml
```

The journal file is `<id>-<incarnation>-<UTC timestamp>.journal`. The id is in
the name because two connections to one exchange would otherwise both write
`kraken-000001-...` and the second would truncate the first when started in the
same second. The exchange tag inside the file header is still `kraken` or
`deribit`, not the id.

A journal holds every symbol its connection subscribed to, interleaved in
arrival order. Anything reading one for a single symbol has to filter.

## Schema

`config/feed_handler.toml` is the committed default (it reproduces what the
binaries used to hardcode) and a test parses it, so it cannot rot.

| Key | Where | Meaning |
|---|---|---|
| `journal_dir`, `state_dir` | top level, optional | default `journal`, `state`, relative to the working directory |
| `id` | connection | lowercase letters, digits, `_`, `-`, at most 48; names the journal files |
| `exchange` | connection | `kraken` or `deribit` |
| `env` | connection | `prod` or `testnet`, required so a connection never silently picks one |
| `feed` | connection, optional | `level3` (Kraken) or `book` (Deribit): the only one each supports |
| `symbols` | connection | non-empty, no duplicates, characters `A-Za-z0-9_-./` only |
| `endpoint` | connection, optional | Kraken: a `ws(s)://` URL. Deribit: `host:port` |
| `api_key_env`, `api_secret_env` | connection | the NAMES of environment variables, never the credentials |

Validation is strict and reports the entry it failed on: unknown keys are
rejected (a `symbol` for `symbols` typo would otherwise capture nothing while
looking healthy), and the credential fields must look like variable names, so
pasting a secret there is refused rather than stored.

The symbol character set is a safety rule, not a style one: symbols are
spliced without escaping into a hand-built JSON string (Kraken) and a
SOH-delimited FIX field (Deribit). Loosening it means adding escaping first.

## Gotchas

- **Endpoint defaults exist only where this project has connected**: Kraken
  prod and Deribit testnet. Deribit prod and Kraken testnet require an explicit
  `endpoint`; the Deribit production FIX host has not been verified here.
- **Kraken connections must be `env = "prod"`**: the token call is a
  production REST call and there is no Kraken testnet REST endpoint to pair a
  different websocket with.
- **Kraken: at most 200 symbols per connection** (validated), and the
  subscribe is one message. The subscribe cost is depth-weighted (5 per symbol
  at depth 10 against a 200/s budget, `decisions/0004`), so a burst past about
  40 symbols would need pacing, which is not implemented. At the intended 5-10
  symbols it does not matter.
- **Kraken acknowledges a multi-symbol subscribe once per symbol.** The client
  logs each as `subscribed to level3 SYM (k/N)`; a rejected symbol does not close
  the connection, so `k < N` in the log is the only sign of a partial capture.
- **Deribit answers with one snapshot per symbol** and has no ack, so the log's
  `received a MarketDataSnapshotFullRefresh for SYM` lines are the check. What
  Deribit does when only one symbol of several is invalid is not confirmed.
- **All Kraken connections share one `RestClient`**, deliberately: it owns the
  nonce high-water mark, and its token fetches are serialized by a mutex
  because the nonce source and HTTP client under it are single-threaded.
- **Every log line carries `[<id>]`**, which is how connections in one process
  are told apart.
