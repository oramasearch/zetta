# M-13 computer vision

The integration in #1082 keeps primary-solution lookup with the catalogue's
first-listed method: S-8 selects portfolio M-7 (anomaly detection) and S-12
selects M-1 (tabular supervised learning). The catalogue uses a different
method numbering, as mapped in zetta-bridge `docs/solutions/buildable-models.md`
§8. Vision remains available by `M-13`, `vision`, and `computer_vision`;
survival and ranking remain available by their own IDs and model kinds.

M-13 is native transfer learning for governed image tables. A frozen Vision
Transformer (ViT) turns each PNG or baseline JPEG into an embedding, and a
distributed linear head learns the table's integer labels. The implementation
uses C++26, Zetta's safetensors reader, and the existing compute-job collectives;
it embeds no Python runtime, foreign ML library, or hosted model call.

## Inputs

The graph's `connect` step names one database and table. Every row must contain:

| Role | Default column | Contract |
|---|---|---|
| image | `image` | large-object id in the table's database |
| label | `label` | non-negative integer class |
| source | `source` | non-empty camera, site, document, or comparable group |

The `image_column`, `label_column`, and `source_column` compile hints change the
names. At least two labels and two source groups are required. Partitioned
tables are read from the partitions owned by the participating member.

Before reading an image body, the runner checks the acting identity's
`blob:read` authorization for that id. One denial refuses the run; the denied
body is never read. Missing or malformed image bodies are excluded with a
warning in the run log and counted in `images_excluded`.

## Native model path

The decoder supports non-interlaced 8-bit PNG images and baseline sequential
JPEG images, with exactly one frame: a JPEG with a second SOF0 is refused (LSR-245, #1418). It converts them to RGB, applies bilinear resize and channel
normalization, forms non-overlapping patches, and runs class-token ViT
self-attention and MLP blocks. All frozen tensors are loaded by name from a
registered `vision_backbone` Model through Zetta's native safetensors reader.

Only the softmax linear head is trained. Every epoch allreduces summed local
gradients and row counts with `JobRound::allreduce`; held-out confusion counts
are combined the same way. Initialization, source assignment, and updates are
deterministic under `seed`. The distributed trainer accepts embeddings from
owned partitions and its tests prove identical weights on multiple members.

The registry runner currently refuses a cluster with more than one observed
member because registry dispatch does not yet co-launch the image decode and
embedding pass on every owner. This prevents a lone participant from entering
collectives. The native trainer itself is distributed; adding the registry
co-launch is deferred.

## Correctness rules

- Train and test are split by complete `source` groups, never by frame. A source
  cannot occur in both sets.
- A vision graph must set `source_disjoint: true` and name a non-empty
  `source_column`. `VisionSourceSplitCheck` blocks a graph that does not.
- Every large-object authorization check happens before its body read. Denied
  images refuse the run instead of becoming exclusions.
- Evaluation uses only the source-disjoint held-out rows. The report carries
  accuracy, macro-F1, and a confusion matrix indexed by actual then predicted
  class.
- The frozen backbone and trained head are native safetensors bodies. No
  checkpoint is downloaded during a test or run.

## Outputs

A successful run registers a `vision` Model containing the linear-head weights,
metrics, backbone reference, ViT configuration, column contract, and confusion
matrix. It also creates
`vision_results.predictions_<run_id>` with:

| Column | Meaning |
|---|---|
| `image` | input large-object id |
| `label` | observed class |
| `predicted_label` | highest-probability class |
| `score` | probability assigned to `predicted_label` |
| `source` | source group |
| `split` | `train` or `test` |

`get_run` exposes the Model, prediction table, and report outputs with lineage.
`get_run_report` reads those artifacts back and returns accuracy, macro-F1,
train/test row counts, images read/excluded, confusion matrix, prediction rows,
and output table references.

## Checkpoints and licences

This change selects no pretrained checkpoint. Unit and journey tests create tiny
random weights of the same architecture under fixed seeds, so there is no
external weights, vocabulary, tokenizer, or configuration licence to record and
no network download.

A production `vision_backbone` Model with pretrained weights must record these
fields in `feature_schema`:

| Field | Required value |
|---|---|
| `vit_config` | the exact tensor-compatible architecture |
| `weights_origin` | `pretrained` |
| `checkpoint_revision` | an immutable revision or commit |
| `weights_license` | `MIT`, `Apache-2.0`, `BSD-2-Clause`, or `BSD-3-Clause` |
| `config_license` | one of the same permitted licences |

ViT uses no tokenizer or vocabulary. The runner refuses missing revisions and
unknown or non-permissive licences before loading weights. Selecting a concrete
checkpoint also requires updating this page with its repository, immutable
revision, weights licence, and configuration licence.

The optional real-checkpoint test is skipped unless both
`ZETTA_VIT_CHECKPOINT` and `ZETTA_VIT_CONFIG` point to local files. It validates
the architecture and tensor names and never downloads them.

## Running it

First register a `vision_backbone` Model with a `vit_config` and safetensors
body. Then compile M-13 against the table:

```json
{
  "project": "inspection",
  "methodology": "M-13",
  "hints": {
    "database": "datasets",
    "table": "inspection_frames",
    "backbone_model": "<model-id>",
    "image_column": "image",
    "label_column": "label",
    "source_column": "camera_id",
    "holdout_fraction": 0.25,
    "seed": 7
  }
}
```

Call `run` with the stored graph. The methodology deliberately reads the
pipeline's `connect` table; a separate `dataset_path` does not replace it.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Vision*:*ViT*:*Image*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:*GraphCheck*:ActionJourneys.*:Uc1Journey.*'
./build/tests/zetta-tests
```

The focused tests cover PNG and JPEG decode, deterministic tiny-ViT embeddings,
a planted separable head signal, source-group isolation, distributed allreduce,
graph refusal, registry compilation, table execution, and report read-back.
