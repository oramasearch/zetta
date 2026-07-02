# Synthetic churn dataset (UC-1 fixture) + loader

The **synthetic churn dataset** (`zetta::ChurnDataset`,
`include/zetta/usecases/churn/churn_dataset.hpp`) is a small, committed, synthetic
(non-customer) churn dataset — **accounts** + **comms/usage events** — that the
native tests load directly, plus a tiny C++ loader. It is the **root task of
FEAT-6** ("learns from your data") for **UC-1 churn** (#402; supports TC-4,
#251): the substrate the later featurization, training, oracle, and end-to-end
tests build on.

**No foreign runtime (ADR-17).** The data is *checked in* under
`tests/fixtures/churn/`; the generator that authored it is **not** a
repo/build/CI dependency. No Python, no XGBoost, no warehouse — just static
files and a C++ reader. This is the zetta way.

## What it carries

Everything the UC-1 `featurize → train → explain` pipeline needs, by
construction:

- an **`as_of_date`** (the decision time `t0`) and a **`churn_horizon_days`**, so
  featurization can window to events with `event_ts <= t0` and the label is a
  forward horizon — a few events are deliberately **after** `t0` (the **leakage
  trap** a correct featurizer must drop);
- a per-account **churn label** (`churned` + a forward `churn_date`);
- the make-or-break **FedEx entity-resolution case** (TC-4): *FedEx Freight* + 5
  sibling subsidiaries (Ground, Express, Office, Logistics, Services) as
  **separate accounts** that must not be conflated.

The dataset is intentionally tiny (10 accounts, 27 events) — large enough to be
realistic, small enough to read.

## On-disk layout

```
tests/fixtures/churn/
  dataset.json        ← the manifest (the churn "contract"): as_of_date, horizon, …
  accounts.jsonl      ← one account per line   (canonical)
  accounts.csv        ← the same accounts, flat (sibling format)
  events.jsonl        ← one comms/usage event per line   (canonical)
  events.csv          ← the same events, flat (sibling format)
```

`dataset.json` is always JSON; the rows come in **JSONL (canonical)** and **CSV**
(see *Formats* below). The JSONL and CSV forms load to the same dataset (a test
asserts it).

## The data contract (manifest)

`dataset.json` declares the small contract UC-1 needs to instantiate churn on any
data — the entity to score, the time column, the label, `t0`, and the horizon:

| field | meaning |
|---|---|
| `name` | dataset name (`synthetic-churn-uc1`) |
| `description` | provenance / intent |
| `entity_field` | the field identifying the unit to score (`account_id`) |
| `time_field` | the event field carrying the timestamp (`event_ts`) |
| `label_field` | the account field carrying the label (`churned`) |
| `as_of_date` | the decision time `t0`; features use only `event_ts <= as_of_date` |
| `churn_horizon_days` | forward label horizon in days |
| `label_definition` | exactly how the label is defined |

## Records

**Account** (`zetta::ChurnAccount`): `account_id`, `name`, `brand`, `entity`,
`plan_tier` (`starter`|`business`|`enterprise`), `industry`, `region`, `mrr`,
`seats`, `tenure_days`, `renewal_date`, `churned`, `churn_date`. `entity` is the
TC-4 ground-truth division; `account_id` is its correct resolution target;
`brand` groups siblings (e.g. `"FedEx"`) that share a brand but are distinct
accounts.

**Event** (`zetta::ChurnEvent`): `event_id`, `account_id`, `event_ts`,
`event_type` (`login`|`feature_use`|`payment_failed`|`support_ticket`|
`csm_contact`|`comms`), `channel`, `mention`, `value`. Every event carries an
`event_ts`; `mention` is non-empty only on `comms` rows — the raw account-name
string a resolver must map to the right account (the entity-resolution case).

## The FedEx entity-resolution case (TC-4)

Six accounts share `brand = "FedEx"` — `acct_fedex_freight`, `…_ground`,
`…_express`, `…_office`, `…_logistics`, `…_services` — each a **distinct
account** with a **distinct `entity`** (`fedex_freight`, …). The `comms` events
carry mention variants (`"FedEx Freight"`, `"Fedex Freight"`, …), plus a
near-collision (`"Umbrella Logistics"` → `acct_umbrella`, *not* FedEx Logistics)
to keep resolution non-trivial.

`zetta::entity_resolution_attribute(dataset, strategy = FullName)` builds the
`entity_resolution` payload by **actually resolving** the comms mentions against
the account directory with [`zetta::EntityResolver`](entity-resolution.md): the
resolver sees only the raw `mention` string and matches it to an account, while the
ground-truth `entity` is held out and recorded so the gate can check the resolver's
choice. With the default `FullName` strategy the resolution is division-correct —
`"FedEx Freight"` and the variant `"Fedex Freight"` both land on
`acct_fedex_freight` while `"FedEx Ground"` stays separate — so the payload passes
`zetta::EntityResolutionCheck` (and `default_gate()`) because it *resolved
correctly*, not by construction. Passing `ResolutionStrategy::Brand` collapses every
`"FedEx …"` mention onto one brand bucket, so the gate **fails and blocks deploy**
(RULE-1) — the real-resolver way to drive the conflation failure. See
[entity-resolution.md](entity-resolution.md) and
[entity-resolution-check.md](entity-resolution-check.md).

## The loader

```cpp
#include "zetta/usecases/churn/churn_dataset.hpp"

zetta::ChurnDatasetResult result = zetta::load_churn_dataset(dir);  // JSONL by default
if (!result.ok()) { /* result.error is a path-qualified reason */ }
const zetta::ChurnDataset& dataset = result.dataset;
```

`load_churn_dataset(dir, format = Jsonl)` reads `dataset.json` plus
`accounts.<ext>` / `events.<ext>` for the chosen `format`. It is **non-throwing**
(returns an `error` string, never throws across the zetta-core seam) and
**schema-validates every record at the boundary** — the first malformed record
fails the load with a path-qualified reason, exactly like the Run record. The
loaded structs are the in-memory frame the featurizer/oracle compute against; the
JSONL files also import straight into a zetta table via
`zetta::import_records(table, dir / "accounts.jsonl")` (so the data is *loadable
into a zetta table*, the acceptance bar).

### Boundary validation

Three draft-07 JSON Schemas define and guard the formats:

- `schemas/churn_dataset.schema.json` — the manifest,
- `schemas/churn_account.schema.json` — one account,
- `schemas/churn_event.schema.json` — one event.

Each is **mirrored by an embedded copy in `src/core/usecases/churn/churn_dataset.cpp`** (the
validator — `pboettch/json-schema-validator` — is PRIVATE to zetta-core, so the
embedded copy is what runs); keep the committed file and the embedded copy in
sync. CSV cells are coerced to the typed JSON shape and validated against the
**same** schema, so both formats share one boundary.

## Formats: JSONL, CSV, and Parquet

`ChurnDataFormat` is `{ Jsonl, Csv, Parquet }` — all three round-trip to the
identical `ChurnDataset`:

- **JSONL** — canonical; round-trips through `import_records` into a zetta table
  with no extra parser.
- **CSV** — a committed flat sibling, read by a small RFC-4180 parser with typed
  coercion; loads to the identical dataset.
- **Parquet** — the **columnar** format, read by zetta's own native (no-Arrow)
  [Parquet reader](../data/parquet-reader.md) (#508): `load_churn_dataset(..., Parquet)`
  reads `accounts.parquet` / `events.parquet` via `zetta::parquet::read_parquet_rows`,
  then runs the rows through the same schema validators + `from_json` mappers as
  the other formats. Still **no foreign runtime** (ADR-17) — the reader is
  hand-rolled, and the committed `.parquet` fixtures are regenerated dev-time by
  `scripts/gen-parquet-fixture.py` (pyarrow), not at build/CI. The
  `ParquetFormatLoadsIdentically` test proves parity with JSONL.

## Regenerating the data

The JSONL + CSV fixture is authored by a throwaway generator that emits both
forms from one source so they can't drift. That generator is **not committed** and
is **not a build/CI dependency** — only its output is checked in (ADR-17). The
**Parquet** siblings (`accounts.parquet` / `events.parquet`) are written **from
the canonical JSONL** by `scripts/gen-parquet-fixture.py` (pyarrow, dev-time only;
also not a build/CI dependency). To change the data, regenerate the JSONL/CSV, then
re-run the parquet generator, and re-run the tests (`JsonlAndCsvLoadIdentically` +
`ParquetFormatLoadsIdentically` guard drift).

## How the tests consume it

`tests/usecases/churn/test_churn_dataset.cpp` exercises the acceptance bar: the fixture loads
and schema-validates; JSONL == CSV; the six FedEx siblings are separate accounts;
events straddle `as_of_date`; accounts import into a zetta `Table`; the
division-correct resolution passes the gate while a conflation blocks deploy; and
malformed records (JSONL and CSV) fail at the boundary. Tests find the fixture
via the `ZETTA_TEST_FIXTURES_DIR` compile-define (`tests/fixtures`).

## Pointers

- [execution-graph.md](../pipeline/execution-graph.md) — the IR the resolution payload rides on.
- [entity-resolution.md](entity-resolution.md) — the resolver (`zetta::EntityResolver`) that maps the fixture's comms mentions to accounts (TC-4 producer).
- [entity-resolution-check.md](entity-resolution-check.md) — the TC-4 check this fixture feeds.
- [gate.md](../pipeline/gate.md) — the correctness gate that blocks deploy on conflation (RULE-1).
- [use-cases/uc1.md](uc1.md) — the full UC-1 churn solution this is the dataset for.
