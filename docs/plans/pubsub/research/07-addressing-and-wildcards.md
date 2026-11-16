# Research: addresses, wildcards, and email-style envelopes

> **Provenance.** One Perplexity (`perplexity_ask`, high context) run,
> 2026-09-27, after the decision to use typed unique names (`topic:abc123`, `agent:405`,
> `table:logs`) with to/cc/bcc/reply-to lists, wildcards and a correlation id.
> **Status:** background; describes no zetta code.

## Wildcard grammars elsewhere

| System | Grammar | Where matching happens |
|---|---|---|
| NATS | `.`-separated tokens; `*` = one token, `>` = the rest; request/reply via a unique `_INBOX.` reply subject [4][8] | subscriptions and permissions; you cannot publish *to* a wildcard |
| MQTT | `/`-separated levels; `+` = one level, `#` = the rest; `$`-topics excluded from leading wildcards [1] | subscription filters only |
| RabbitMQ topic exchange | `.`-separated words; `*` = one word, `#` = zero or more [2] | bindings, evaluated per message |
| Kafka | regular expressions | subscriptions, which also match topics created later |
| Akka | wildcards in actor paths (`ActorSelection`) | the sender selects actors |

**What is new in our design:** a *sender* can address a wildcard
(`to: agent:security-*`), like Akka, not only a subscriber. And our wildcard is a
**prefix glob inside a name** (`security-*`), which NATS/MQTT/RabbitMQ
deliberately do not support (their `*` is a whole token).

## Email semantics (RFC 2822 [3])

- **To**: primary recipients; **Cc**: informational recipients; **Bcc**:
  recipients whose addresses are **not shown** to the other recipients [3][7].
- **Reply-To**: a list of addresses replies should go to; not correlation.
- **Message-ID / In-Reply-To / References**: identify a message, its parent, and
  the thread chain: correlation and threading.

## Pitfalls

- **Snapshot vs standing match:** expanding `agent:*` at send time reaches the
  agents that exist *now*; a standing pattern also reaches agents created later.
  Pick one per case.
- **Authorization of wildcard sends:** a permission to send to `agent:*` is a
  permission to reach everyone; govern it separately from sending to one name [4].
- **Fan-out explosion:** a broad pattern multiplies delivery work; cap it or
  report its match count.
- **A reply-to address proves nothing** about who sent the message; identity
  comes from governance, not from the envelope [4][8].

## Sources

[1] https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/csprd01/mqtt-v3.1.1-csprd01.doc ·
[2] https://www.rabbitmq.com/docs/exchanges ·
[3] https://www.rfc-editor.org/rfc/rfc2822.html ·
[4] https://docs.nats.io/learn/security/authorization ·
[7] https://learn.microsoft.com/da-dk/previous-versions/office/developer/exchange-server-2010/aa493889(v=exchg.140) ·
[8] https://docs.nats.io/learn/core-nats/request-reply
