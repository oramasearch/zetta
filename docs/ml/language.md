# M-10 language: native embeddings, retrieval, grounded answers, and tuning

M-10 is the native language methodology tracked by
[#1049](https://github.com/oramasearch/zetta/issues/1049) under the
[methodology epic](https://github.com/oramasearch/zetta/issues/984). It turns a
governed table of text chunks into versioned embeddings, retrieves relevant
chunks for labelled queries, produces grounded answers, and fits a linear
classifier head over the embeddings. It uses C++26 only: no Python process,
foreign ML runtime, or hosted model API.

## Encoder and licence

The enabled encoder is
[`sentence-transformers/all-MiniLM-L6-v2`](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2)
at immutable revision
`1110a243fdf4706b3f48f1d95db1a4f5529b4d41`. The repository declares
Apache-2.0. That licence covers the exact checkpoint's safetensors weights,
`config.json`, `vocab.txt`, and tokenizer configuration stored together in the
checkpoint repository. Apache-2.0 passes the methodology's permissive-licence
gate.

The loader requires these local files:

- `config.json`
- `vocab.txt`
- `model.safetensors`
- `zetta_checkpoint_revision.txt`, containing exactly
  `sentence-transformers/all-MiniLM-L6-v2@1110a243fdf4706b3f48f1d95db1a4f5529b4d41`

The revision marker prevents a different checkpoint from being silently
reported as the pinned encoder. Unit-test checkpoints use a
`tiny-random-bert:<seed>` marker and contain randomly initialized weights of the
same BERT block shape.

The native WordPiece tokenizer adds `[CLS]` and `[SEP]`, applies greedy
longest-match continuation pieces, and truncates at the configured position
limit. The encoder implements BERT embeddings, bidirectional multi-head
attention, GELU feed-forward blocks, residual connections, and LayerNorm using
Zetta's autograd tensor operations and safetensors reader. Sentence embeddings
use the checkpoint's mean pooling and L2 normalization, producing 384 values for
the real checkpoint.

`intfloat/multilingual-e5-small` is the documented multilingual alternative,
but is not enabled by this runner. An implementation must pin and re-check the
exact checkpoint licence before loading it. E5 retrieval text must be prefixed
with `query: ` for queries and `passage: ` for indexed chunks; omitting those
prefixes changes the representation the checkpoint was trained to produce.

## Inputs

The connect step names the run's own `database.table`. A run accepts the same
resolved dataset contract as other native methodologies:

- `entity_column`: integer chunk id;
- `feature_columns[0]`: text column, unless `text_column` overrides it;
- `label_column`: numeric classifier label;
- `query_column`: labelled retrieval query, default `query`;
- `relevant_id_column`: planted relevant chunk id, default `relevant_id`.

Rows without an integer id or string text are excluded and counted. Queries or
planted ids may be absent; they are then not included in recall@k or MRR.

## Outputs and metrics

Each successful run registers a `language` Model and replaces three tables in
`language_results` for that graph:

- `embeddings_<graph>`: chunk id, source, text, normalized vector, and exact
  `embedder_version`;
- `retrieval_<graph>`: query, rank, chunk id, cosine score, source, and relevance;
- `answers_<graph>`: query, answer, refusal, cited chunk ids, and groundedness.

The model weights body is the fitted linear classifier head. It records the
embedder version it was trained against. Embeddings are recomputed on every run
and every row records that version, so a changed encoder cannot reuse an older
vector or classifier head unnoticed.

The run report exposes:

- `recall_at_k`: fraction of labelled queries whose relevant id appears in the
  first `top_k` results;
- `mrr`: mean reciprocal rank of the planted relevant chunk;
- `groundedness`: fraction of non-refused answer sentences carrying a retrieved
  chunk citation; the enforced target is `1.0`;
- rows read, rows excluded, labelled training rows, output tables, and dataset
  notes.

Classifier sufficient statistics are summed with `JobRound::allreduce`; the
three-member test proves partitioned statistics produce the same versioned head
as a single-node oracle. Encoding, retrieval, and answer generation are local
operations. The registry's current multi-member adapter reads the resolved table
through distributed SQL before those operations; co-launching the classifier
job on every table owner remains outside this slice and is called out in the
runner beside the fallback.

## Correctness rules

The following rules are code-enforced:

1. `LanguageTrainingAccessCheck` blocks a graph unless its source and model
   declare caller-read enforcement. At run time, M-10 checks the acting user's
   read scope for every training column before reading any rows.
2. `LanguageGroundingCheck` blocks a graph unless citations are required and an
   empty or irrelevant retrieval must be refused.
3. The answer path refuses when no hit clears `minimum_relevance`. Every sentence
   of a non-refused answer receives a citation to a retrieved chunk.
4. The checkpoint revision is explicit, vectors are normalized, and model/table
   metadata carries the embedder version.
5. A seed controls native Qwen sampling. WordPiece, BERT inference, retrieval
   ordering, and classifier reduction are deterministic for fixed inputs.

When `qwen_directory` is supplied, the prompt and retrieved chunks go through
Zetta's existing native Qwen loader, tokenizer, and generation path. Without it,
the safe fallback is extractive: it returns text from the top retrieved chunk
and still applies the same relevance, refusal, and citation enforcement.

## Running it

Land a table, create a project, capture an intent, then compile M-10 with the
table in the shape hints:

```json
{
  "project": "support",
  "methodology": "M-10",
  "hints": {"dataset_path": "knowledge.chunks"}
}
```

Run the compiled graph with the pinned local checkpoint and an explicit text
contract:

```json
{
  "project": "support",
  "graph": "grounded answers",
  "embedder_directory": "/models/all-MiniLM-L6-v2",
  "dataset": {
    "entity_column": "id",
    "label_column": "label",
    "feature_columns": ["text"]
  },
  "top_k": 5,
  "classifier_regularization": 0.1,
  "seed": 1049
}
```

Read the returned `run_id` with `get_run` and `get_run_report`. The latter is the
authoritative retrieval and grounding report and names the output tables.

## Verification

The default test path creates no network traffic and downloads no weights. It
uses tiny random BERT weights and covers deterministic normalized embeddings, a
planted retrieval hit, empty-retrieval refusal, sentence citations, distributed
classifier statistics, table/model/lineage outputs, graph refusals, and the
registry journey.

The real-checkpoint test is skipped unless `ZETTA_MINILM_DIR` names a prepared
directory with the four files above:

```sh
ZETTA_MINILM_DIR=/models/all-MiniLM-L6-v2 \
  ./build/tests/zetta-tests --gtest_filter=LanguageEmbed.RealMiniLmCheckpoint
```

The methodology proof is:

```sh
./build/tests/zetta-tests --gtest_filter='*Embed*:*Rag*:*Retriev*:*Language*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
```
