# Events, addresses and the envelope

**Issue:** [#1025](https://github.com/oramasearch/zetta/issues/1025) · **Decisions:**
D4, D26, D27, D29, D33, D34 ([../plans/pubsub/decisions.md](../plans/pubsub/decisions.md))
· **Code:** `include/zetta/pubsub/address.hpp`, `include/zetta/pubsub/event.hpp`,
`src/core/pubsub/{address,event}.cpp` · **Tests:** `tests/pubsub/test_events.cpp`

## Addresses

Everything an event can be sent to has a unique `kind:name`: `topic:orders`,
`agent:405`, `table:logs`. `parse_address` validates at the boundary (kind
`[a-z][a-z0-9_-]*`, a non-empty name with no whitespace or `:`) and says why it
rejected. A name ending in `*` is a **wildcard**, a prefix glob inside the name:
`agent:*`, `agent:security-*`. `address_matches(pattern, target)` requires equal
kinds.

## The envelope

| Field | Meaning |
|---|---|
| `from` | the principal the event is for (stamped by the node, never trusted from a caller) |
| `sender` | the delegation stack of agents that sent it, oldest first; at most 10,000 deep |
| `to`, `cc`, `bcc` | recipients; `copy_for_recipient` strips every bcc address except the recipient's own |
| `reply_to` | where replies go |
| `correlation_id` | ties a reply to its request |
| `topics_chain` | topics the event passed through, newest last |

`check_sender_chain` rejects a stack over 10,000 (or a lower configured limit, never
a higher one). `check_topic_hop` refuses a topic already on the chain (a loop) and a
chain at its depth limit (default 32).

## Payloads

`Payload::make(bytes, inline_threshold)` keeps a payload inline when it is at most
the threshold (default 256 bytes) and otherwise puts it once on the heap as an
immutable, reference-counted buffer, so fan-out never copies it. Encodings are
`blob`, `json`, `text`; MIME types are interned to a 16-bit id by `MimeRegistry`
(id 0 = none).

## On-disk frame

`encode_event_frame` / `decode_event_frame` write one record as
`[u32 length][u32 crc32][u32 header length][JSON header][payload]`, little-endian.
A short buffer decodes as `Incomplete` (a torn tail), a checksum or header mismatch
as `Corrupt`; neither is ever parsed into an event. Used by spilling and the
shutdown flush.
