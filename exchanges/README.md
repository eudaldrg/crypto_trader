# Exchange reference notes

One file per exchange, holding **facts about the exchange's own protocol** —
endpoints, auth flow, message shapes, field meanings, quirks confirmed by
probing. This is reference material, not rationale: *why* we picked a given
feed or format lives in `decisions/*.md` instead. When a probe in
`experiments/` turns up something new about how an exchange actually
behaves on the wire, capture the finding here (and cross-link from the
relevant ADR if it affected a decision), rather than letting it live only in
a commit message or chat history.

Keep entries factual and dated where the fact could plausibly change
(rate limits, endpoint hosts, undocumented behavior) — exchanges alter
public API details without much notice.

- [`kraken.md`](kraken.md)
- [`deribit.md`](deribit.md)
