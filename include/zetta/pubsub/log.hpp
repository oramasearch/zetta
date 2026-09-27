#pragma once

#include <functional>
#include <string_view>

// Pub/sub logging (decisions D11, D12, D18, D20, D24, D34). Every detected
// default, spill, dispatch switch, dropped event and shutdown outcome is
// written through a LogSink — an injection point, so tests capture the lines
// and the node routes them wherever its logs go. The default sink writes
// "pubsub: LEVEL message" to stderr.
namespace zetta::pubsub {

enum class LogLevel { Info, Warn, Error };

std::string_view to_string(LogLevel level);

using LogSink = std::function<void(LogLevel, std::string_view)>;

// Writes "pubsub: INFO|WARN|ERROR <message>" to stderr.
LogSink stderr_log_sink();

}  // namespace zetta::pubsub
