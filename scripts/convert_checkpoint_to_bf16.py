#!/usr/bin/env python3
"""Convert a checkpoint's model.safetensors from F32 to BF16, halving the
on-disk weight bytes so the agent's cold load reads half as much.

BF16 here is a plain truncation to the high 16 bits of each F32 — which is
exactly how zetta's readers widen it back:

  - compute/safetensors.cpp  (the MLX serving path, MlxQwenInference)
  - autograd/safetensors.cpp (the CPU fallback: "BF16 is the high 16 bits of
    the f32; widen and reinterpret")

so a round trip (F32 -> high16 -> read back with low16=0) is bit-exact with the
truncated value the reader reconstructs. Greedy decode on the 0.6B agent is
unaffected in practice (verified live: coherent replies, correct trained
"Correct solution (facts)" format).

What does NOT change: warm tokens/sec. Decode runs on F32 weights held in
memory (MLX up-converts on load), so BF16 only shrinks the file and the
weight-read portion of the cold load — not generation. Fixed cold-load costs
(the ~11 MB tokenizer.json BPE parse, config reads, MLX graph build + Metal
shader warmup) are unaffected, so the wall-clock win is bounded to the weight
read (measured: ~2.38 GB F32 cold ~7.9 s -> ~1.19 GB BF16 cold ~5.2 s).

Usage:
    python3 scripts/convert_checkpoint_to_bf16.py [SRC_DIR] [DST_DIR]

Defaults: SRC = data/checkpoints/base-agent-0.6b,
          DST = SRC + "-bf16". The F32 original is never modified. Point an
agent at DST with update_agent(id, model=DST) (the gitignored checkpoint dir
stays local; this script is the reproducible part). Stdlib + numpy only.
"""
import json
import os
import shutil
import struct
import sys

import numpy as np

DEFAULT_SRC = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "data", "checkpoints", "base-agent-0.6b")

# Sidecar files an inference checkpoint needs beside model.safetensors.
SIDECARS = ("config.json", "generation_config.json",
            "tokenizer.json", "tokenizer_config.json")


def convert(src_dir: str, dst_dir: str) -> None:
    os.makedirs(dst_dir, exist_ok=True)
    with open(os.path.join(src_dir, "model.safetensors"), "rb") as handle:
        header_len = struct.unpack("<Q", handle.read(8))[0]
        header = json.loads(handle.read(header_len))
        data = handle.read()

    new_header: dict = {}
    blob = bytearray()
    offset = 0
    converted = 0
    for name, info in header.items():
        if name == "__metadata__":
            continue
        dtype, shape = info["dtype"], info["shape"]
        begin, end = info["data_offsets"]
        raw = data[begin:end]
        if dtype == "F32":
            arr = np.frombuffer(raw, dtype=np.float32)
            bf16 = (arr.view(np.uint32) >> 16).astype("<u2")  # high 16 bits
            raw = bf16.tobytes()
            dtype = "BF16"
            converted += 1
        new_header[name] = {"dtype": dtype, "shape": shape,
                            "data_offsets": [offset, offset + len(raw)]}
        blob += raw
        offset += len(raw)
    if "__metadata__" in header:
        new_header["__metadata__"] = header["__metadata__"]

    header_json = json.dumps(new_header, separators=(",", ":")).encode("utf-8")
    with open(os.path.join(dst_dir, "model.safetensors"), "wb") as handle:
        handle.write(struct.pack("<Q", len(header_json)))
        handle.write(header_json)
        handle.write(bytes(blob))

    for sidecar in SIDECARS:
        src = os.path.join(src_dir, sidecar)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(dst_dir, sidecar))

    src_size = os.path.getsize(os.path.join(src_dir, "model.safetensors"))
    dst_size = os.path.getsize(os.path.join(dst_dir, "model.safetensors"))
    print(f"tensors converted F32->BF16: {converted}")
    print(f"{src_size / 1e9:.2f}GB -> {dst_size / 1e9:.2f}GB "
          f"({100 * dst_size / src_size:.0f}%)")
    print(f"BF16 checkpoint: {dst_dir}")


if __name__ == "__main__":
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    dest = sys.argv[2] if len(sys.argv) > 2 else source + "-bf16"
    convert(source, dest)
