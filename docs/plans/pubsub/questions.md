# Open questions

Answered questions stay here with their answer, date and who answered; the answer
is also carried into the page it governs.

## Round 1 — 2026-09-27

Steve answered in one message, which settled [D1–D4](decisions.md) and moved the
design past several of these questions. Still open from round 1:

1. **What is the first thing that will publish, and the first thing that will
   subscribe?** Still open; it sets the defaults (inline size, segment size).
2. **Broadcast, work queue, or both?** → *Both, and more, chosen at runtime* (D3).
3. **When a ring is full?** → *Chain another ring* (D2). What happens when
   memory is the limit is Q13.
4. **Ordering per topic or per key?** → Reordering is a runtime choice (D3);
   per-key ordering still open.
5. **What is a message?** → *Binary with an encoding tag: blob + MIME type, JSON,
   …; heap for events too big for the ring* (D4).
6. Topics static in code or named at runtime and listable? Open.
7. Does a new subscriber see only new events, or the last N? Open.
8. Exposed on MCP/REST in stage 1, or engine-internal first? Open.
9. Lock-free a goal? → *No; locks and condition variables are fine* (D1).

## Round 2 — 2026-09-27

10. ~~**How do coroutine subscribers wait?**~~ → *Answered 2026-09-27 (Steve):
    don't use coroutines for this; subscribers wait on condition variables, with
    a mutex per queue, deque or consumer queue* ([D5](decisions.md)). Original
    question: A coroutine that blocks on a
    `condition_variable` holds a pool thread. Options: (a) thread subscribers use
    the condition variable, coroutine subscribers get a waiter list that
    `publish` posts to their executor (both, one mutex); (b) condition variable
    only, and coroutine code calls it through `run_blocking` on a pool thread;
    (c) condition variable only, and we accept the blocked thread.
11. ~~**Where are delivery semantics chosen?**~~ → *Answered 2026-09-27
    (Steve): at any level, or several* ([D6](decisions.md)). Original question:
    per topic, per subscription, or per message? E.g. is "deliver to one" a property of a subscriber group (like
    NATS queue groups), or can a single publish say "this one goes to 3
    recipients"?
12. **`std::queue` can't do broadcast.** It only has `front`/`pop`, so it can
    feed one reader. Is the `std::queue` store meant for work-queue topics only,
    or should the alternative store be a `std::deque` used by index?
13. ~~**Is growth ever capped?**~~ → *Answered 2026-09-27 (Steve): yes,
    configurable at all levels with sane defaults, potentially overflowing to
    disk* ([D10](decisions.md)). Original question: **Is growth ever capped?** A slow or dead subscriber pins every ring after
    its cursor. Options: no cap; a per-topic memory limit after which the
    slowest subscriber is dropped or skipped ahead and told it missed N; or
    fall back to blocking the publisher.
14. **Heap events: immutable and refcounted, released when the last entitled
    subscriber passes them?** And is "too big for the ring" a per-topic
    inline threshold (e.g. 256 bytes), or literally larger than a whole ring
    segment?
15. **Which encodings in stage 1, and is the MIME type interned** to a small id
    in the header, or stored as a string per event?
16. ~~**What does QoS mean?**~~ → *Answered 2026-09-27 (Steve): both priority
    and the delivery guarantee, configurable at all levels* ([D9](decisions.md)).
    Original question: **What does QoS mean for you: priority (who goes first), or a delivery
    guarantee** (at-most-once vs at-least-once with acknowledgement and
    redelivery, like MQTT QoS 0/1)? Or both?

## Round 3 — 2026-09-27

17. ~~**When levels disagree, which wins?**~~ → *Answered 2026-09-27 (Steve):
    the security/governance rules defined on the topic, user and subscriber,
    through the central governance model of users, roles, grants, rules and
    permissions* ([D7](decisions.md), [`governance.md`](governance.md)).
    Original question: Options: (a) the most specific wins
    (message over subscription over topic); (b) the topic and subscription set
    limits and a message can only narrow within them, the way MQTT delivers at
    the lower of the publish QoS and the subscription QoS; (c) a different rule
    per axis (e.g. recipients: message wins; priority: message wins; guarantee:
    the minimum).

## Round 4 — 2026-09-27

18. ~~**Over-bound requests: reject or clamp?**~~ → *Answered 2026-09-27
    (Steve): configurable per topic, clamp by default* ([D8](decisions.md)).
    Original question: **When a publish or subscription asks for more than governance allows** (e.g.
    priority 9 where the role's bound is 5, or "deliver to everyone" where it may
    only use "one"), do we (a) **reject** it with an error naming the rule, and
    audit the denial (zetta's deny-by-default), or (b) **clamp** it to the bound,
    deliver, and audit that it was clamped?

## Round 5 — 2026-09-27

19. **Are these the right defaults?** → *Steve, 2026-09-27: too small; a
    partition can see more than 10k events per second.* Revised in the stage 1
    page (limits in bytes, sized from a throughput target); the target itself
    is Q20. Original proposal: Proposed in
    [`stage-1-in-memory-fifo.md`](stage-1-in-memory-fifo.md#limits-and-overflow-d10):
    per-topic memory cap 64 MiB; a subscription may fall 10,000 events behind;
    when full, the default policy follows the guarantee: **at-most-once skips the
    slow subscriber ahead** (it is told how many it missed), **at-least-once and
    exactly-once spill to disk** up to 1 GiB per topic, then **hold the publisher**
    for up to 5 s, then fail the publish. Spilling is off for topics whose events
    must never touch disk.

## Round 6 — 2026-09-27

20. ~~**What throughput should the defaults be sized for?**~~ → *Answered
    2026-09-27 (Steve): don't guess; auto-select from the environment (pod
    requests/limits, system memory total/used, …), overridable by configuration,
    and log what was detected* ([D11](decisions.md)). Q19 is closed by the same
    answer. Original question: Proposed target: **100k
    events/s per partition at ~1 KiB each (~100 MiB/s)**. The revised defaults
    follow from it: 1 GiB of memory per partition (~10 s of traffic), 32 GiB of
    disk per partition (~5 min), both inside node-wide budgets (25% of RAM, 50% of
    free disk). If the target is higher, or events are bigger, the defaults scale
    with it.

## Round 7 — 2026-09-27

21. ~~**Partitions.**~~ → *Answered 2026-09-27 (Steve): we might implement
    partitions in the future, but not now* ([D13](decisions.md)). Original
    question: **Partitions.** You said "per partition": is a topic partitioned, like a
    Kafka topic, with each partition its own ordered ring? If so, what picks an
    event's partition (a key hash, round robin, the publisher naming it), and is
    ordering guaranteed only within a partition?

## Round 8 — 2026-09-27

22. ~~**Are topics named at runtime and listed like tables?**~~ → *Answered
    2026-09-27 (Steve): yes, and persisted to disk and reloaded after any
    shutdown, graceful or not; persistent topics can be defined; topics can also
    be auto-created implicitly from configuration, like Kafka* ([D14–D16](decisions.md)).
    Original question: **Are topics named at runtime and listed like tables?** E.g. `CREATE TOPIC
    orders` over SQL (or a `create_topic` capability over MCP/REST), visible in
    `system.pubsub_topics`, governed by `pubsub:manage`, and replicated so every
    node knows the topic exists (its events stay in memory on one node for now).
    Or declared in C++ code only, for engine-internal use, in stage 1?

## Round 9 — 2026-09-27

23. ~~**Default fsync policy for a persistent topic?**~~ → *Answered 2026-09-27
    (Steve): by default events persist to disk on shutdown too; only a
    non-default setting makes a topic discard on shutdown (never using disk);
    more persistent topics come later; for now topics are memory-only except for
    restarts, crashes and overflow* ([D17](decisions.md)). Original question:
    **What is the default fsync policy for a persistent topic** (how much a crash
    may lose)? Options, from research/05: (a) **every event** before `publish`
    returns: no loss, lowest throughput (RabbitMQ confirms, Redis `always`);
    (b) **on an interval**, e.g. every 1 s: lose up to that interval (Redis
    `everysec`; NATS defaults to 2 min); (c) **leave it to the OS**: highest
    throughput, loss depends on the OS (Kafka's usual setting, which relies on
    replication). Presumably configurable at every level like the rest; the
    question is the default.

## Round 10 — 2026-09-27

24. ~~**Which crash must a default topic survive?**~~ → *Answered 2026-09-27
    (Steve): don't worry about persistence now; best effort, and log the
    outcome; persistent topics are designed later* ([D18](decisions.md)).
    Original question: **Which crash must a default topic survive?** Proposal (research/06): keep
    ring segments in **memory-mapped files** in the data directory. Writes stay
    memory writes; if the **process** dies (kill -9, segfault, OOM kill), the
    kernel still holds the pages and writes them back, so nothing is lost and no
    fsync is paid. If the **machine** dies (power loss, kernel panic, node
    failure), whatever the kernel had not written back is lost. Is surviving a
    process crash enough for the default, with machine-failure durability left
    to the later durable topics? Or must the default also survive machine
    failure (which needs periodic fsync, i.e. D15's policy question)?

## Round 11 — 2026-09-27

7. ~~*(reopened from round 1)* **Where does a new subscriber start?**~~ →
   *Answered 2026-09-27 (Steve): configurable at all levels; by default it joins
   the subscriber list and receives events as its rules define*
   ([D19](decisions.md)). Original: Proposal:
   at the **next** event published (default), configurable to the **oldest
   event still held**, or to a given sequence number.

## Round 12 — 2026-09-27

8. ~~*(reopened from round 1)* **Is stage 1 exposed on MCP/REST**~~ →
   *Answered 2026-09-27 (Steve): optionally; not exposed externally by default*
   ([D22](decisions.md)). Original: **Is stage 1 exposed on MCP/REST**, or engine-internal
   first? Proposal: both in the same change, per zetta's rule that a capability
   ships on the API and MCP with the engine work: `create_topic`, `drop_topic`,
   `publish`, and `subscribe` streaming events over SSE (the MCP transport
   already uses it), with the in-process C++ API underneath for engine code.

Settled without asking, under the standing rule (Steve may overrule):
Q12 ~~the second store is an indexable `std::deque`~~ — *corrected by Steve
(D21): the deque is the holder of ring buffers inside the `TopicRingStack`, not
a second store*; Q14 heap events are immutable and refcounted, and the inline threshold
is a per-topic setting defaulting to 256 bytes; Q15 stage 1 encodings are
`blob` (with MIME type), `json` and `text`, with MIME types interned to a 16-bit
id per node.

## Round 13 — 2026-09-27

25. ~~**Inside the `TopicRingStack`, which lock guards an append or a read?**~~
    → *Answered 2026-09-27 (Steve): the queue has its own lock and condition
    variables; a topic might have a few queues (producer, internal, consumer) to
    simplify locking, by configuration* ([D23](decisions.md)). Original: (a)
    the **stack's mutex** for everything: one lock per topic, simplest, but every
    publisher and subscriber of a topic contend on it; or (b) the stack's mutex
    only for adding/removing rings, and **each ring has its own mutex** for
    appends and reads, so readers of an older ring never contend with the
    publisher writing the newest. Lock order in (b): stack before ring.


## Round 14 — 2026-09-27

26. ~~**What moves events along the chain**~~ → *Answered 2026-09-27 (Steve):
    configurable; default a thread per topic, moving to a shared pool past a
    topic-count threshold or when heuristics trigger; inline is also an option*
    ([D24](decisions.md)). Original: **What moves events along the chain** (producer queue → internal queue →
    consumer queues)? (a) a **dispatcher thread per topic**: simple, one thread
    per topic; (b) a **shared pool** of dispatcher threads serving every topic:
    bounded threads, needs fairness between topics; or (c) **no dispatcher**: the
    producer's own thread carries its event through every hop (no extra thread,
    but a slow consumer queue slows the publisher).

## Round 15 — 2026-09-27

1. ~~*(reopened from round 1)* **What will publish first, and what will subscribe
   first?**~~ → *Answered 2026-09-27 (Steve): anything can, especially agents;
   agent tasks will use pub/sub too, designed elsewhere* ([D25](decisions.md)).
   Original: **What will publish first, and what will subscribe first?** The design is now general enough that the first real use decides
   what stage 1 builds first and what its tests look like. Candidates seen in
   zetta: agent events for Ask Orama / Train → Agents, pipeline run progress,
   table change events, cluster signals, the GUI's live activity feed.

## Round 16 — 2026-09-27

27. ~~**Request/reply.**~~ → *Answered 2026-09-27 (Steve): every topic and
    agent has a unique name (`topic:abc123`, `agent:405`, `table:logs`);
    reply-to is a list of those, with a correlation id; addresses can be
    wildcards (`agent:*`, `agent:security-*`); events also have to, cc and bcc*
    ([D26–D27](decisions.md)). Original: **Request/reply.** Agent tasks usually need an answer: "do this task, send me
    the result". Should pub/sub support it directly, the way NATS does (a publish
    carries a **reply-to** topic and a **correlation id**; the requester waits for
    the reply with a timeout; a short-lived reply topic is created and dropped
    automatically)? Or is that left to the agent-task layer, built from plain
    topics?

## Round 17 — 2026-09-27

28. ~~**What does delivering to a non-topic address mean?**~~ → *Answered
    2026-09-27 (Steve): yes, an inbox per entity* ([D28](decisions.md)); plus
    `from`, `topics-chain`, cached wildcards and inbox retention ([D29–D31](decisions.md)).
    Original: **What does delivering to a non-topic address mean?** Proposal: every
    addressable entity has an **inbox**, a queue of its own. `to: agent:405` puts
    the event in agent 405's inbox (its consumer queue, D23); `to: topic:abc123`
    publishes to that topic, whose subscribers get it by their rules. And
    `to: table:logs`: does it **insert the event as a row** into the table (the
    table as a sink), or does something subscribed on the table's behalf
    receive it?

## Round 18 — 2026-09-27

29. ~~**Where do undeliverable events go?**~~ → *Answered 2026-09-27 (Steve):
    the dead-letter proposal, configurable at all levels* ([D32](decisions.md)).
    Original: **Where do undeliverable events go?** Several cases now produce them: an
    inbox whose retention expired with events still in it (D31), an event whose
    time to live ran out (D10), an address or wildcard that matches nobody, a
    loop or chain-depth stop (D29), a subscriber skipped ahead (D10). Proposal: a
    **dead-letter address** (`topic:dead-letter` by default, settable at every
    level), receiving the event with a reason; or simply drop and log.

## Round 19 — 2026-09-27

30. ~~**What does `from` say when an agent acts for a person?**~~ → *Answered
    2026-09-27 (Steve): both, `from` (the person) and `sender` (the agent),
    configurable at all levels* ([D33](decisions.md)).

31. ~~**Delegation chains.**~~ → *Answered 2026-09-27 (Steve): check the full
    stack; 10,000 is the maximum, and at that point drop with an error in the
    log* ([D34](decisions.md)). Original: **Delegation chains.** Ask Orama can delegate to a subagent, which can
    delegate again. When `agent:security-scan` sends on behalf of
    `agent:ask-orama`, which acted for `user:steve`: is `sender` just the last
    agent, or a **stack** of every agent in the chain (like `topics-chain`), each
    link checked by governance and recorded in the audit? And is there a maximum
    delegation depth?

## Round 20 — 2026-09-27

32. ~~**What drains a table's inbox?**~~ → *Answered 2026-09-27 (Steve): insert
    as a row by default; a Postgres function or stored procedure is another
    target (its own address, not `table:`); also services, model inference, etc.*
    ([D35](decisions.md)). Original: **What drains a table's inbox?** `to: table:logs` puts an event in the
    table's inbox (D28). By default, is each event **inserted as a row** (the
    envelope and payload mapped onto the table's columns, rejected to the dead
    letters when they don't fit the schema), or does the inbox only hold events
    until something subscribed on the table's behalf reads them?

## Round 21 — 2026-09-27

33. **Where does a drain's result go?** A `function:` call returns a value, a
    `model:` inference returns a prediction, a `table:` insert returns the new
    row (or an error). Proposal: the result is sent automatically to the event's
    `reply-to` addresses with the same `correlation-id` (nothing is sent when
    `reply-to` is empty), and a failure is sent there too as an error event, as
    well as to the dead letters.
