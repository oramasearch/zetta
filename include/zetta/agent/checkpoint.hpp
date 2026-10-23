#pragma once

#include <optional>
#include <string>

namespace zetta::agent {

// Resolve an exact operator-registered selector before any serving loader or cache lookup
// (#1411). Defaults: base-agent and /data/models/base-agent. ZETTA_AGENT_CHECKPOINTS is a
// JSON object extending/overriding those selectors with absolute checkpoint directories.
// Registered trees must be operator-controlled and immutable while serving. Symlinks and
// shard references outside the registered directory are refused. See docs/agent/checkpoints.md.
std::optional<std::string> checkpoint_directory(const std::string& selector,
                                               std::string* error = nullptr);

}  // namespace zetta::agent
