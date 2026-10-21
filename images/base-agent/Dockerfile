# syntax=docker/dockerfile:1.7
#
# The Orama base model as a model image (lake#246): the checkpoint under /model and a POSIX
# shell, the contract of orama-operator's OramaZettaCluster spec.models (docs/zetta-models.md).
# Every Lake workspace Zetta copies it to /data/models/base-agent before it starts, and a native
# agent created without a model loads it from there.
#
# The build context holds one directory, checkpoint/, with the files of one published version of
# OramaSearch/base-agent-0.6b (README.md says how to fetch it). Nothing else goes in the image.
FROM busybox:1.37
COPY --chmod=0755 checkpoint/ /model/
# A checkpoint the engine cannot load must not become an image: the native loader needs the
# config and the weights, and the BPE tokenizer needs tokenizer.json or vocab.json + merges.txt.
RUN test -f /model/config.json \
 && { test -f /model/model.safetensors || test -f /model/model.safetensors.index.json; } \
 && { test -f /model/tokenizer.json || { test -f /model/vocab.json && test -f /model/merges.txt; }; }
USER 10001
