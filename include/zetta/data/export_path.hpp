// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// **The export root: the one directory a caller may have the node write files in**
// (#1397, #1399).
//
// Two capabilities write a file on the node at a path the caller names:
// `generate_dataset`'s `path` (the gated JSONL corpus) and `submit_feedback`'s
// `export_path` (the feedback bundle). Before this, each opened the path as given
// with a truncating write, so a caller could overwrite any file the engine's OS
// account can write: its own security files, catalog state, configuration.
//
// Now the path is **relative to `<data_dir>/exports`**, and `write_export_file`:
//
//   * refuses, before touching the filesystem, everything `resolve_import_path`
//     refuses lexically (empty, absolute, leading `~`, backslash, NUL, any `..`);
//   * opens the export root without following a link (the node creates it, mode
//     0700, beside `imports/` and `security/`, never inside either);
//   * walks each directory component with `openat(..., O_NOFOLLOW | O_DIRECTORY)`,
//     creating a missing one (mode 0700) and refusing a symbolic link;
//   * creates the file itself with `O_CREAT | O_EXCL | O_NOFOLLOW` (mode 0600), so
//     an existing file or link is never truncated or followed: an export never
//     overwrites anything, including an earlier export;
//   * removes a partly written file when the write fails.
//
// Trusted in-process callers (the desktop app, the CLI, tests) still write where
// they like; the confinement is at the API boundary, where the name comes from a
// caller.

namespace zetta { class Node; }

namespace zetta::data {

// The export root of a node: `<data_dir>/exports`.
std::filesystem::path export_root(const Node& node);

struct ExportResult {
    std::string relative;         // the written path, relative to the export root, when ok()
    std::filesystem::path path;   // the same, absolute, when ok()
    std::string error;            // why nothing was written; names the requested path
    bool ok() const { return error.empty(); }
};

// Write `bytes` to a new file at `requested` (relative, no `..`, no links) under
// `root`. Never overwrites. Non-throwing.
ExportResult write_export_file(const std::filesystem::path& root, std::string_view requested,
                               std::string_view bytes);

// The same, under `export_root(node)`.
ExportResult write_export_file(const Node& node, std::string_view requested,
                               std::string_view bytes);

}  // namespace zetta::data
