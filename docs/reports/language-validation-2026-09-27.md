# M-10 language validation — 2026-09-27

## Version and environment

- Branch: `issue-1049-methodology-m-10`
- Base revision: `307356c5c5cc4e1acf43bba1bf149cd2eb515705`
- Platform: macOS arm64, Release configuration, Clang 22.1.8
- Checkpoint fixture: `tiny-random-bert:77`, generated locally with the native
  safetensors writer; no weights were downloaded

The fixture keeps the BERT-family shape while reducing it to one encoder layer,
eight hidden values, two attention heads, and a sixteen-value feed-forward
layer. The end-to-end run used two labelled chunks and `top_k = 1`.

## Measured results

| Measure | Result |
| --- | ---: |
| Retrieval recall@1 | 1.0 |
| Answer groundedness | 1.0 |
| Rows read | 2 |
| Rows excluded | 0 |
| Distributed classifier members | 3 |
| Distributed labelled rows | 3 |

The planted relevant chunk ranked first for both labelled queries. Every
non-refused sentence carried a retrieved chunk citation. Empty retrieval was
refused. The three-member classifier reduction matched the single-node oracle
exactly and every member returned the same versioned head.

## Verification record

The Release target built successfully with six workers. The focused language
proof ran 19 tests: 18 passed and the real-checkpoint test was skipped because
`ZETTA_MINILM_DIR` was not set. The registry, journey, and graph-check proof ran
35 tests and all passed. Schema regeneration ran the complete 2,124-test binary;
2,110 tests passed, 13 optional tests were skipped, and one unrelated
distributed-ingest test failed in the combined run before passing alone at a
one-minute host load of 3.54.

The normal complete run reported 2,109 passes, 13 optional skips, and two
unrelated failures. Both
`DistributedIngest.ANonSnowflakeConfigNeverTakesTheSnowflakeProtocol` and
`ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad` passed when rerun
alone at a one-minute host load of 14.20. The latter is also the known
development-branch failure tracked by issue #959.

The optional shared-library configuration reached the link stage but the
untouched Beast2 dynamic target omitted the Corosio OpenSSL library, leaving
`boost::corosio::openssl_stream` symbols undefined. The static target contains
the M-10 implementation and completed all methodology proofs.

## Limits

The exact pinned `sentence-transformers/all-MiniLM-L6-v2` checkpoint was not
available locally, so its skipped-by-default validation did not run. The loader
still requires the immutable revision marker and rejects a mismatched revision.
The registry adapter uses distributed SQL to read owned data but completes the
classifier head centrally; the independently tested classifier job performs
its sufficient-statistics reduction with `JobRound::allreduce` across three
members.
