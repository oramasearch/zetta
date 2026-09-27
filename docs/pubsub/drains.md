# Drains, replies, dead letters and error topics

**Issue:** [#1032](https://github.com/oramasearch/zetta/issues/1032) · **Decisions:** D32,
D35, D36, D37, D38 · **Code:** `include/zetta/pubsub/pubsub.hpp` (`InboxDrain`,
`CallbackDrain`), `src/core/pubsub/pubsub.cpp` · **Tests:** `tests/pubsub/test_pubsub.cpp`

## Drains (D35)

An address whose kind has a registered `InboxDrain` (`register_drain(kind, drain)`) is
handled by that drain on the dispatcher: `table:` inserts a row, `function:` /
`procedure:` calls, `model:` infers, `service:` delivers. The list is open: a new kind is
a new drain, never a special case. A drain returns a `DrainResult` (ok or an error, and
an optional result). A drain that throws is reported as an error, never taken down with
the dispatcher.

## Results (D36)

Where a result goes: the message's `delivery.reply`, else the drain's `default_reply()`.
The global default is nowhere; a query drain defaults to `ReplyTo` (the event's
`reply_to` list, with the same `correlation_id`), an insert to `None`. `ReplyAll` also
sends to every `to` and `cc`, never `bcc`. A reply comes `from` the drain's address.

## Dead letters (D32)

Undeliverable events go to `topic:dead-letter` with a `reason` header: no such address,
no wildcard match, a topic loop or chain depth, a time to live that ran out, an inbox
whose retention ran out. A dead letter never expires and is never dead-lettered again
(it is dropped and logged).

## Error topics (D37)

A recipient's failures go to `topic:errors.<kind>.<name>`, created on its first error,
with `error` and `recipient` headers and the original envelope and correlation id.

No rate limits in stage 1 (D38).
