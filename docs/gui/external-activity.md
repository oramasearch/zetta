# External activity surfacing — the GUI half of the activity hook

The observability half of the control-surfaces thesis (#620; engine half
shipped with the registry, #615/#644): when an agent (MCP) or integration
mutates the cluster, the operator watching the headed app sees it as it
happens. The wtm embedded-server pattern: any successful `mutates: true`
registry call fires `Registry::set_activity_hook`
(`include/zetta/api/registry.hpp` — `ActivityEvent{capability, actor}`, on the
owning loop); the headed app subscribes and surfaces it.

## Why "refresh" is free — and what the GUI actually adds

There are no dirty flags to set: every panel reads live sources each frame
(`system.*` views, `Node::…()` — the thin-renderer rule), MCP handlers execute
on the UI thread during the bridge drain (`App::set_tick_hook` →
`ApiServer::drain()`, once per frame iteration), and the idle loop re-renders
at ~30 fps (`SDL_WaitEventTimeout`). So an external mutation is applied and
rendered within the same frame iteration — an MCP-triggered churn run animates
in the Cluster view and Graph Workspace with zero extra wiring (#575/#590/#626
read the live records each frame), and a remote deploy or dataset add is in
its panel on the next render. Likewise the activity feed (#622) needs nothing:
every capability call is already audited, and the feed recomputes from the
audit trail + runs each frame.

What the operator *lacked* was noticing: nothing said "an agent just did
that". The GUI half is that affordance.

## The subscription (main.cpp → App)

Headed `main` wires the hook to `App::notify_external_activity(capability,
actor)` — a public, thread-safe method: it only appends to a mutex-guarded
queue (`pending_external_activity_`). Today the hook fires on the owning
(UI) thread inside the drain; the queue keeps the contract honest if a future
transport fires it elsewhere. The hook is cleared before the App is destroyed
(the server outlives it slightly).

`App::poll_background_ops()` (once per frame, any view) drains the queue into
presentation state `recent_external_activity_` — stamped with the ImGui
clock, pruned after a **15 s linger**, capped at 6 entries. No engine state
lives in the GUI: the durable record is the call's audit event, already in
the activity feed.

## The strip line

`draw_dashboard()` (the persistent metadata strip above every view) renders
the newest unexpired entry as one quiet mono line:

```
● MCP · Create project · local · just now
```

- `status_dot(Tone::Active)` + the registry entry's **title** (product
  language; the raw capability name only as a fallback), the actor (`local`
  or `key <first-8>`), and the age.
- The line is an `entity_link` deep-linking via `App::navigate_to(view, {})`
  to the panel that owns what changed — **click-through only, never
  auto-navigation** (the never-yank-the-view rule):
  `create_project`/`capture_intent`/`import_n8n` → Explorer · `compile_*` →
  Graph workspace · `deploy` → Deploy · `run`/`testsuite_run` → Runs ·
  `add_dataset` → Datasets · anything else → Activity
  (`App::view_for_external_capability`, app_home.cpp).
- With more than one unexpired entry, a `View all activity` link follows.

Failed mutating calls never surface (the registry fires the hook only on
success); their audit record still reaches the feed.

## Palette suppression

The Cmd+K palette executes the same registry (one list, many renderings), so
its own synchronous `Registry::call` would fire the hook too.
`run_palette_capability` sets `palette_call_in_flight_` for the duration of
the call and `notify_external_activity` drops events while it is set — the
strip reports **external** actors only. GUI-native actions (buttons, the
governed console) don't go through the registry and need no suppression.

## Boundaries

- **No new JSON format, no new stored state** — the hook carries two strings;
  everything durable was already replicated + audited.
- `ActivityEvent` carries no entity id yet, so deep links select the owning
  panel without a row selection; threading the result's entity ref through
  the event is a later slice.
- REST-initiated calls (#618) will ride the same hook unchanged; the strip
  label follows the feed's surface naming (`MCP` until the renderings are
  distinguishable).
