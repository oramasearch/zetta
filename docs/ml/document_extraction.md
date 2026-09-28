# Document extraction (M-11)

M-11 turns landed documents into auditable field rows. It is native C++ under
ADR-17: PDF text recovery has no PDF dependency, image preprocessing and patch
embedding run in Zetta, TrOCR weights use the native safetensors reader, and no
hosted model or foreign ML runtime participates.

## Inputs

The runner reads a resolved `database.table`. Each row may contain:

- `document_id`: stable document identity;
- `document`: plain text, PDF bytes with a text layer, or P2/P5 graymap bytes;
- `labels`: optional object mapping declared field names to expected values.

The run declares `fields`, and may override `document_column`,
`document_id_column`, `labels_column`, `review_threshold`, `seed`, and
`trocr_directory`. The first image boundary is portable graymap so decoding
stays dependency-free. Other image formats must be converted before landing.
Encrypted PDFs and PDFs without literal `Tj`/`TJ` text operators yield no text
regions. Image-only PDF pages must be rasterized to P2/P5 graymap bytes before
landing; the runner never invents text for an unsupported PDF stream.

## Processing and outputs

PDF text operators retain page and approximate text coordinates. Scans are
normalized, divided into horizontal ink regions, projected into image patches,
and recognized by an encoder-decoder. Declared fields are selected from exact
key-value pairs first, then nearby or fuzzy keys. M-10 embeddings can replace
the edit-distance fallback once its runner is available.

The run writes two tables under `document_results`:

- `extracted_fields_<graph>` contains `document`, `field`, `value`,
  `confidence`, `page`, `box`, and `review`;
- `review_queue_<graph>` contains the same evidence rows for every result below
  the configured threshold.

The registered `document_extraction` model reports field precision, recall,
F1, review share, documents read and excluded, and field counts. The durable
run links the input table, both output tables, the model, and the report.

## Correctness rules

- Every field has a finite confidence clamped to `[0, 1]`.
- A result below `review_threshold` is placed in the review table. It is never
  silently accepted.
- Every result retains a one-based source page and source box for audit.
- The graph gate refuses an M-11 model without confidence, retained evidence,
  low-confidence routing, or an explicit review-queue action.
- A fixed seed produces identical tiny-model tokens, text, and confidence.
- Labelled rows are compared at field level. A wrong non-empty value is both a
  false positive and a false negative; a missing value is a false negative.

Document byte recovery and recognition are local operations. Metric counts are
expressed as allreduce-ready sufficient statistics. The current registry
adapter runs only when one member owns the resolved table and refuses a
multi-member partial result; a coordinated owned-partition launch remains
deferred rather than reporting incomplete fields.

## Checkpoint and licences

The supported reference checkpoint is
[`microsoft/trocr-base-handwritten`](https://huggingface.co/microsoft/trocr-base-handwritten)
at immutable revision
`eaacaf452b06415df8f10bb6fad3a4c11e609406`. The repository metadata records
the exact checkpoint as MIT. The `model.safetensors` weights, `config.json`,
`preprocessor_config.json`, `tokenizer_config.json`, `vocab.json`, and
`merges.txt` are distributed together under that MIT repository licence. This
is permissive under the issue's licence gate.

Production loading requires a local snapshot directory containing
`config.json`, `model.safetensors`, and `vocab.json`. Tests never download
weights. `DocumentExtraction.RealCheckpointRunsWhenLocalSnapshotIsProvided`
is skipped unless `ZETTA_TROCR_DIR` names an already provisioned snapshot.

## Running it

Land a table, create a project, and compile with `methodology: M-11` (or
`solution: document_automation`). Hints set the source and declared fields:

```json
{
  "project": "accounts-payable",
  "methodology": "M-11",
  "name": "invoice extraction",
  "hints": {
    "source": "documents",
    "table": "invoices",
    "fields": ["invoice_number", "date", "total"],
    "review_threshold": 0.8
  }
}
```

Call `run` with the compiled graph, poll `get_run`, then read
`get_run_report`. The report returns the model metrics, input row counts, and
both output-table descriptors. REST and MCP expose the same registered
capabilities.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Document*:*Ocr*:*Pdf*:*Extraction*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```

The focused tests generate a PDF text layer with planted fields, execute a tiny
random encoder-decoder twice under one seed, prove review routing, validate the
patch-embedding gradient, and drive compile-to-report through the registry.
