#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate THIRD_PARTY_LICENSES: every component written by others, with its license text.

    python3 tools/third-party-licenses.py [--npm LABEL=DIR ...] [--out THIRD_PARTY_LICENSES]

The C and C++ components (submodules, FetchContent downloads, vendored files and
fonts) are listed in COMPONENTS below; keep that list in step with .gitmodules and
the CMake files. A submodule's version is read from the commit this checkout pins.
License texts come from the file named in the entry, or from GitHub at the pinned
version through the `gh` command line (it must be signed in; private forks need
access).

JavaScript bundles are read from an installed dependency tree: run
`npm ci --ignore-scripts --omit=dev` in the directory first, then pass it as
`--npm LABEL=DIR`. Each package's license comes from license-checker-rseidelsohn,
run with `npx`, and its text from the package's own license file.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys

# ---------------------------------------------------------------------------
# The repository's components. Fields:
#   name, license (SPDX), use (how it reaches the product), url,
#   version (or "submodule": path, read from the pinned commit),
#   text: {"github": "owner/repo", "path": ..., "ref": optional, default the version;
#          None for the default branch} | {"file": path} | {"inline": text}
# ---------------------------------------------------------------------------
REPOSITORY = "zetta"
COMPONENTS: list[dict] = [
    {"name": "Boost", "version": "1.91.0", "license": "BSL-1.0",
     "use": "fetched at build (CMake FetchContent), linked",
     "url": "https://github.com/boostorg/boost",
     "text": {"github": "boostorg/boost", "ref": "boost-1.91.0", "path": "LICENSE_1_0.txt"}},
    {"name": "capy", "submodule": "modules/capy", "license": "BSL-1.0",
     "use": "submodule, linked", "url": "https://github.com/cppalliance/capy",
     "text": {"github": "cppalliance/capy", "path": "LICENSE"}},
    {"name": "corosio (Orama's fork of cppalliance/corosio)", "submodule": "modules/corosio",
     "license": "BSL-1.0", "use": "submodule, linked",
     "url": "https://github.com/cppalliance/corosio",
     "text": {"github": "oramasearch/corosio", "path": "LICENSE"}},
    {"name": "http (Orama's fork of cppalliance/http)", "submodule": "modules/http",
     "license": "BSL-1.0", "use": "submodule, linked",
     "url": "https://github.com/cppalliance/http",
     "text": {"github": "oramasearch/http", "path": "LICENSE"}},
    {"name": "beast2", "submodule": "modules/beast2", "license": "BSL-1.0",
     "use": "submodule, linked", "url": "https://github.com/cppalliance/beast2",
     "text": {"github": "cppalliance/beast2", "path": "LICENSE"}},
    {"name": "libpqxx", "submodule": "modules/libpqxx", "license": "BSD-3-Clause",
     "use": "submodule, linked", "url": "https://github.com/jtv/libpqxx",
     "text": {"github": "jtv/libpqxx", "path": "COPYING"}},
    {"name": "oramacore-cpp (Orama Search Inc., released with this stack)",
     "submodule": "modules/oramacore-cpp", "license": "MIT", "use": "submodule, linked",
     "url": "https://github.com/oramasearch/oramacore-cpp", "text": {"file": "LICENSE"}},
    {"name": "automerge-cpp (Steve Sperandeo)", "submodule": "modules/automerge-cpp",
     "license": "MIT", "use": "submodule, linked", "url": "https://github.com/homer6/automerge-cpp",
     "text": {"github": "homer6/automerge-cpp", "path": "LICENSE"}},
    {"name": "nlohmann/json (vendored by automerge-cpp, upstream/json)",
     "version": "8167d2f64122c3ef7e5a0d9cab239fdba37aa54c", "license": "MIT",
     "use": "header-only, through automerge-cpp", "url": "https://github.com/nlohmann/json",
     "text": {"github": "nlohmann/json", "path": "LICENSE.MIT"}},
    {"name": "json-schema-validator", "version": "2.4.0", "license": "MIT",
     "use": "fetched at build, linked", "url": "https://github.com/pboettch/json-schema-validator",
     "text": {"github": "pboettch/json-schema-validator", "path": "LICENSE"}},
    {"name": "SDL", "version": "release-3.2.0", "license": "Zlib",
     "use": "fetched at build, linked into the desktop app", "url": "https://github.com/libsdl-org/SDL",
     "text": {"github": "libsdl-org/SDL", "path": "LICENSE.txt"}},
    {"name": "Dear ImGui", "version": "v1.91.5", "license": "MIT",
     "use": "fetched at build, linked into the desktop app", "url": "https://github.com/ocornut/imgui",
     "text": {"github": "ocornut/imgui", "path": "LICENSE.txt"}},
    {"name": "GoogleTest", "version": "v1.15.2", "license": "BSD-3-Clause",
     "use": "fetched at build, tests only", "url": "https://github.com/google/googletest",
     "text": {"github": "google/googletest", "path": "LICENSE"}},
    {"name": "IBM Plex Sans and IBM Plex Mono (assets/fonts)", "version": "as vendored",
     "license": "OFL-1.1", "use": "vendored font files, embedded in the binaries",
     "url": "https://github.com/IBM/plex",
     "text": {"github": "IBM/plex", "ref": None, "path": "LICENSE.txt"}},
    {"name": "Geist and Geist Mono (assets/fonts, webapp/fonts)", "version": "as vendored",
     "license": "OFL-1.1", "use": "vendored font files", "url": "https://github.com/vercel/geist-font",
     "text": {"file": "assets/fonts/OFL.txt"}},
    {"name": "Space Grotesk (webapp/fonts)", "version": "as vendored", "license": "OFL-1.1",
     "use": "vendored font files", "url": "https://github.com/floriankarsten/space-grotesk",
     "text": {"github": "floriankarsten/space-grotesk", "ref": None, "path": "OFL.txt"}},
    {"name": "Orama Studio web app (webapp/, built from orama-design)", "version": "fc2cc41",
     "license": "MIT (to be confirmed)", "use": "vendored build, served by the node",
     "url": "https://github.com/oramasearch/orama-design",
     "text": {"inline": "Orama Search Inc.; to be released under MIT with this stack (license to be confirmed before release). The runtime packages of that build are listed below as \"runtime dependency of the webapp/ build\"."}},
]
NPM_USE = {"webapp": "runtime dependency of the webapp/ build"}
NOTES: list[str] = [
    "System libraries, linked but not part of this source tree: OpenSSL (Apache-2.0), zlib",
    "(Zlib) and libpq (PostgreSQL License), from the operating system or its package manager.",
    "Optional backends, off by default and also taken from the system: MLX (MIT, Apple) and the",
    "NVIDIA CUDA Toolkit (cudart, cuBLAS; NVIDIA's CUDA EULA). A binary or image that includes",
    "any of them carries their notices.",
    "The model files under tests/fixtures/model_import are made by",
    "scripts/gen-model-import-fixtures.py from synthetic rows; they are this project's own.",
]
# --- end of the repository's list ------------------------------------------

RULE = "=" * 80
THIN = "-" * 80


def run(cmd: list[str], **kw) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def submodule_commit(path: str) -> str:
    out = run(["git", "ls-tree", "HEAD", path]).split()
    return out[2] if len(out) >= 3 else "unknown"


def github_text(repo: str, ref: str | None, path: str | None) -> str:
    query = f"?ref={ref}" if ref else ""
    if path:
        return run(["gh", "api", "-H", "Accept: application/vnd.github.raw",
                    f"repos/{repo}/contents/{path}{query}"])
    data = json.loads(run(["gh", "api", f"repos/{repo}/license"]))
    return base64.b64decode(data["content"]).decode("utf-8", "replace")


def component_text(c: dict, version: str) -> str:
    src = c["text"]
    if "inline" in src:
        return src["inline"]
    if "file" in src:
        with open(src["file"], encoding="utf-8") as fh:
            return fh.read()
    ref = src["ref"] if "ref" in src else version
    return github_text(src["github"], ref, src.get("path"))


def npm_components(label: str, directory: str) -> list[dict]:
    out = run(["npx", "--yes", "license-checker-rseidelsohn@4", "--production", "--json",
               "--start", directory])
    data = json.loads(out)
    root = json.load(open(os.path.join(directory, "package.json")))
    root_id = f"{root.get('name')}@{root.get('version')}"
    items = []
    for pkg_id, info in sorted(data.items()):
        if pkg_id == root_id:
            continue
        name, _, version = pkg_id.rpartition("@")
        text = ""
        if info.get("licenseFile") and os.path.basename(info["licenseFile"]).lower() not in (
                "package.json", "readme.md", "readme"):
            with open(info["licenseFile"], encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        lic = info.get("licenses")
        lic = " OR ".join(lic) if isinstance(lic, list) else str(lic)
        items.append({
            "name": name, "version": version, "license": lic,
            "use": NPM_USE.get(label, f"bundled ({label})"),
            "url": info.get("repository") or f"https://www.npmjs.com/package/{name}",
            "_text": text or f"(No license file in the package; it declares {lic}.)",
        })
    return items


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--npm", action="append", default=[], metavar="LABEL=DIR")
    parser.add_argument("--out", default="THIRD_PARTY_LICENSES")
    args = parser.parse_args()
    os.chdir(run(["git", "rev-parse", "--show-toplevel"]).strip())

    items = []
    for c in COMPONENTS:
        version = submodule_commit(c["submodule"]) if "submodule" in c else c["version"]
        items.append({**c, "version": version, "_text": component_text(c, version)})
    npm_sources = []
    for spec in args.npm:
        label, _, directory = spec.partition("=")
        npm_sources.append(label)
        items.extend(npm_components(label, os.path.abspath(directory)))

    head = run(["git", "rev-parse", "--short", "HEAD"]).strip()
    cmd = "python3 tools/third-party-licenses.py" + "".join(
        f" --npm {s}=<installed {s} tree>" for s in npm_sources)
    lines = [
        f"Third-party software in {REPOSITORY}",
        "",
        f"{REPOSITORY} is licensed under the MIT License (see LICENSE). It includes or uses the",
        "components below, each under its own license. This file is generated; do not edit it",
        "by hand.",
        f"Generated with: {cmd}",
        f"From: .gitmodules, the CMake files and tools/third-party-licenses.py at {head}"
        + (f"; npm trees: {', '.join(npm_sources)}" if npm_sources else ""),
        "",
    ]
    for note in NOTES:
        lines.append(note)
    if NOTES:
        lines.append("")
    cols = [("Component", "name"), ("Version", "version"), ("License", "license"),
            ("How it is used", "use"), ("Source", "url")]
    widths = [max(len(h), *(len(str(i[k])) for i in items)) if items else len(h)
              for h, k in cols[:-1]]
    lines.append("Summary")
    lines.append("")
    lines.append("  ".join(h.ljust(w) for (h, _), w in zip(cols[:-1], widths)) + "  Source")
    for i in items:
        lines.append("  ".join(str(i[k]).ljust(w) for (_, k), w in zip(cols[:-1], widths))
                     + "  " + str(i["url"]))
    lines.append("")
    for i in items:
        lines += [RULE, f"{i['name']} {i['version']} — {i['license']}", str(i["url"]), THIN,
                  i["_text"].rstrip(), ""]
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines).rstrip() + "\n")
    print(f"third-party-licenses: {len(items)} components written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
