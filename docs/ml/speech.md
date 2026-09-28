# Speech and audio analytics

M-14 transcribes consented recordings entirely inside Zetta. It decodes RIFF/WAVE
PCM, resamples to 16 kHz, finds speech regions with a short-window energy
detector, produces Whisper-compatible log-mel features, and runs the encoder and
autoregressive decoder in native C++26. Timestamp tokens refine segment bounds.

No foreign ML runtime, embedded Python, hosted model, or network inference is
used. Audio samples and transcript text do not enter collective messages. Each
owner processes its own partition and only numeric counters for word errors,
recordings, and segments are summed through `JobRound::allreduce`.

## Inputs and outputs

The connect table has configurable `recording`, `audio`, `consent`, and optional
`reference_text` fields. `audio` is a WAV large-object id in the same database.
`consent` must exist, be boolean, and be `true` before the blob is read. The
reference transcript is used to calculate word error rate.

The durable transcript table contains exactly `recording`, `segment`, `start`,
`end`, `text`, and `confidence`. Its text is privacy-filtered before the row is
added. The registered model records word error rate, edit and reference-word
counts, recordings read and excluded, and segment count. The run links the
source table and checkpoint in lineage; `get_run_report` returns the metrics and
transcript table. This regular table can feed M-10 as text input.

## Correctness rules

The graph gate and runner both enforce these rules:

1. Explicit consent is required on every source row before audio bytes are read.
2. Email addresses, card-like digit groups, and other numbers are replaced
   before transcript text is stored.
3. Audio remains on its owner node. Only aggregate numeric statistics cross a
   distributed collective.

Registry execution refuses a multi-member partial launch because the API
callback is invoked on one member. The statistics job supports coordinated
multi-member launches and is tested across owned partitions. Runs are
deterministic for a fixed checkpoint, input order, and seed.

## Checkpoints and licences

Pretrained checkpoints are accepted only when `zetta-checkpoint.json` names one
of these exact revisions and records permissive licences for every artifact:

| Architecture | Repository | Pinned revision | Weights | Tokenizer / vocabulary | Config |
|---|---|---|---|---|---|
| Whisper tiny | `openai/whisper-tiny` | `169d4a4341b33bc18d8881c4b69c2e104e1cc0af` | Apache-2.0 | Apache-2.0 | Apache-2.0 |
| Whisper base | `openai/whisper-base` | `e37978b90ca9030d5170a5c07aadb050351a65bb` | Apache-2.0 | Apache-2.0 | Apache-2.0 |

The upstream `openai/whisper` implementation is MIT. Zetta does not execute it.
It reads `config.json`, `model.safetensors`, and BPE tokenizer files through its
native config, safetensors, tokenizer, and transformer code. Any other revision
or any licence outside MIT, Apache-2.0, BSD-2-Clause, or BSD-3-Clause is refused.

The manifest beside a local checkpoint is:

```json
{
  "repository": "openai/whisper-tiny",
  "revision": "169d4a4341b33bc18d8881c4b69c2e104e1cc0af",
  "weights_license": "Apache-2.0",
  "tokenizer_license": "Apache-2.0",
  "config_license": "Apache-2.0"
}
```

Unit tests use deterministic random tensors with the released encoder/decoder
block structure and tensor names. They never download weights.
`WhisperCheckpoint.RealPinnedCheckpointIsSkippedByDefault` documents the real
checkpoint test and is skipped unless a reviewer provisions the pinned files.

## Running it

Land a table and register a `speech_backbone` model whose `feature_schema`
contains `whisper_config`, checkpoint origin, and licence metadata. Then call
`compile_intent` with `methodology: M-14` and hints like these:

```json
{
  "database": "media",
  "table": "recordings",
  "checkpoint_model": "<model-id>",
  "recording_column": "recording",
  "audio_column": "audio",
  "consent_column": "consent",
  "label_column": "reference_text"
}
```

Call `run` with that graph, poll `get_run`, then call `get_run_report`. A local
checkpoint directory may be supplied as `checkpoint_directory` instead.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Speech*:*Whisper*:*Mel*:*Audio*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```

The focused suite covers WAV decoding and resampling, a fixed log-mel reference,
deterministic random-weight decoding, privacy filtering, WER, multi-member
aggregation, all three gate refusals, and the registry journey.
