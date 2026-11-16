// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// **The import root: the one directory a caller may name files under** (#1391, #1433).
//
// A caller that names a file on the node — `add_dataset`'s `path`, the directory form
// of `dataset_path` on `run` / `train_model` / `retrain_model`, `testsuite_run`'s
// `custom_data_path` — names it **relative to `<data_dir>/imports`**, and nothing
// else on the node's filesystem is reachable that way. Before this, those paths were
// opened as given, with the node's own file access, so a caller holding
// `orchestration:add_dataset` could import `<data_dir>/security/cluster-secret.json`
// as table rows and read the cluster join credential back.
//
// `resolve_import_path` refuses, before touching the filesystem:
//
//   * an empty path, an absolute path, a path starting with `~`, a backslash or a
//     NUL byte;
//   * any `..` component, wherever it is.
//
// Then it walks the path one component at a time from the import root with
// `openat(..., O_NOFOLLOW)`, so a symbolic link at any step (a directory on the way,
// or the entry itself) is refused rather than followed, and checks the final entry is
// the kind asked for (a regular file, or a directory). Last, the real path of what it
// found must still lie under the real path of the root.
//
// The node's security directory (`<data_dir>/security`: the cluster secret, the root
// secret, the TLS key) and its identity files sit beside `imports/`, never under it,
// so no accepted path reaches them.
//
// Trusted in-process callers (the desktop app's file picker, the CLI, tests) still
// call `add_dataset_file` / `load_churn_dataset` with any path: the confinement is at
// the API boundary, where the path comes from a caller.

namespace zetta { class Node; }

namespace zetta::data {

// The import root of a node: `<data_dir>/imports`. Operators stage files here
// (a volume mount, `kubectl cp`, a sidecar); the node never creates or writes it.
std::filesystem::path import_root(const Node& node);

enum class ImportEntry { File, Directory };

struct ImportPathResult {
    std::filesystem::path path;  // absolute, under the real import root, when ok()
    std::string error;           // why it was refused; names the requested path, never the root's contents
    bool ok() const { return error.empty(); }
};

// Resolve `requested` (relative, no `..`, no symbolic links) under `root`, expecting
// an entry of kind `expected`. Non-throwing.
ImportPathResult resolve_import_path(const std::filesystem::path& root, std::string_view requested,
                                     ImportEntry expected);

// The lexical rules every caller-named path passes before the filesystem is touched,
// shared by the import root and the export root (export_path.hpp): not empty, no NUL,
// no backslash, no leading `~`, not absolute, no `..` component, and not the root
// itself. Returns the path's components (empty and `.` components dropped), or an
// empty vector with `error` naming `where` and the requested path.
std::vector<std::string> caller_path_components(std::string_view requested,
                                                std::string_view where, std::string& error);

// The same, under `import_root(node)`.
ImportPathResult resolve_import_path(const Node& node, std::string_view requested,
                                     ImportEntry expected);

}  // namespace zetta::data
