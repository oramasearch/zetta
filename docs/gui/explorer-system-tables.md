# Explorer: the `system` table list comes from the engine catalog

**Issue:** [#684](https://github.com/oramasearch/zetta/issues/684) · **Code:**
`src/gui/app_explorer.cpp` (`App::draw_tables`), `src/core/query/executor.cpp`
(`kSystemTableCatalog`), `include/zetta/query/executor.hpp`
(`system_table_names()`)

The headed explorer's database panel lists the virtual `system` database.
Selecting it shows **every system table the engine serves** — previously the
list was a hard-coded single entry (`nodes`) while the engine had grown to 20+
tables, all browsable only via hand-typed SQL.

## How it works

- zetta-core owns one catalog array (`kSystemTableCatalog` in
  `src/core/query/executor.cpp`): every system table's name + one-line
  description, alphabetical. From it derive, with no copies:
  - the **`system.tables` read view** — the catalog as a queryable table
    ([data/query.md](../data/query.md), RULE-7);
  - the public accessor **`zetta::query::system_table_names()`**
    (`include/zetta/query/executor.hpp`) — a `std::span` over the names;
  - the unknown-system-table **error suggestions** (the pre-#684 hand-kept
    string had already drifted).
- `App::draw_tables` renders `system_table_names()` each frame — the
  thin-renderer rule: the GUI never owns the name list. Selecting a table
  feeds the existing generic rows panel (`draw_rows` runs
  `SELECT * FROM system.<t>` and renders the result), which needed no change.
- Adding a system table = one entry in `kSystemTableCatalog` + its dispatch
  branch in `Executor::execute`; it then appears in the explorer, in
  `system.tables`, and in the error suggestions at once. A test
  (`tests/data/test_query.cpp`, `EveryCatalogedSystemTableIsSelectable`)
  asserts every cataloged name actually dispatches.

## Boundaries

No new JSON format — `system.tables` rows are computed name/description pairs,
never stored or parsed. `system.nodes` still needs a wired `Cluster`; selecting
it without one surfaces the executor's reason in the rows panel banner,
verbatim, like any other query error.
