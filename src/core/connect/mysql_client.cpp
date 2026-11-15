// SPDX-License-Identifier: MIT

#include "zetta/connect/mysql_client.hpp"

#include "write_words.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/util/sha1.hpp"
#include "zetta/util/sha256.hpp"

// The MySQL client/server protocol, sans I/O (#895). See mysql_client.hpp for
// the shape; the packet layouts follow the MySQL protocol documentation
// ("Client/Server Protocol" in the MySQL source docs): HandshakeV10,
// SSLRequest, HandshakeResponse41, AuthSwitchRequest, AuthMoreData,
// COM_QUERY's text result set, OK/ERR/EOF.
namespace zetta::connect {
namespace {

// Capability flags (the subset this client uses or checks).
constexpr std::uint32_t kClientLongPassword = 0x00000001;
constexpr std::uint32_t kClientLongFlag = 0x00000004;
constexpr std::uint32_t kClientConnectWithDb = 0x00000008;
constexpr std::uint32_t kClientLocalFiles = 0x00000080;
constexpr std::uint32_t kClientProtocol41 = 0x00000200;
constexpr std::uint32_t kClientSsl = 0x00000800;
constexpr std::uint32_t kClientTransactions = 0x00002000;
constexpr std::uint32_t kClientSecureConnection = 0x00008000;
constexpr std::uint32_t kClientMultiStatements = 0x00010000;
constexpr std::uint32_t kClientPluginAuth = 0x00080000;
constexpr std::uint32_t kClientPluginAuthLenencData = 0x00200000;

// utf8mb4_general_ci — present on MySQL 5.5+ and every MariaDB, so text columns
// arrive as UTF-8 whatever the table's own charset is.
constexpr std::uint8_t kCharsetUtf8mb4 = 45;
// The binary pseudo-charset: the column holds bytes, not text.
constexpr std::uint16_t kCharsetBinary = 63;
constexpr std::uint32_t kMaxPacketSize = 16u * 1024u * 1024u;
// A single logical packet larger than this is refused rather than buffered: a
// connector row this large is not something to land as a dataset cell.
constexpr std::size_t kMaxLogicalPacket = 256u * 1024u * 1024u;
constexpr std::size_t kFrameContinuation = 0xffffff;

constexpr std::uint8_t kComQuit = 0x01;
constexpr std::uint8_t kComQuery = 0x03;

// Column types (enum_field_types).
enum : std::uint8_t {
    kTypeDecimal = 0x00, kTypeTiny = 0x01, kTypeShort = 0x02, kTypeLong = 0x03,
    kTypeFloat = 0x04, kTypeDouble = 0x05, kTypeLongLong = 0x08,
    kTypeInt24 = 0x09, kTypeYear = 0x0d, kTypeBit = 0x10,
    kTypeNewDecimal = 0xf6, kTypeTinyBlob = 0xf9, kTypeMediumBlob = 0xfa,
    kTypeLongBlob = 0xfb, kTypeBlob = 0xfc, kTypeVarString = 0xfd,
    kTypeString = 0xfe, kTypeGeometry = 0xff, kTypeVarchar = 0x0f,
};
constexpr std::uint16_t kFlagUnsigned = 0x0020;

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

std::string xor_bytes(std::string_view left, std::string_view right) {
    std::string out(left.size(), '\0');
    for (std::size_t index = 0; index < left.size() && index < right.size(); ++index) {
        out[index] = static_cast<char>(static_cast<unsigned char>(left[index]) ^
                                       static_cast<unsigned char>(right[index]));
    }
    return out;
}

std::string sha256_bytes(std::string_view bytes) {
    const std::string hex = util::sha256_hex(bytes);
    std::string out(hex.size() / 2, '\0');
    for (std::size_t index = 0; index < out.size(); ++index) {
        unsigned value = 0;
        std::from_chars(hex.data() + index * 2, hex.data() + index * 2 + 2, value, 16);
        out[index] = static_cast<char>(value);
    }
    return out;
}

// --- Little-endian wire helpers -------------------------------------------------

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void put_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

void put_lenenc_int(std::string& out, std::uint64_t value) {
    if (value < 0xfb) {
        out.push_back(static_cast<char>(value));
    } else if (value <= 0xffff) {
        out.push_back(static_cast<char>(0xfc));
        put_u16(out, static_cast<std::uint16_t>(value));
    } else if (value <= 0xffffff) {
        out.push_back(static_cast<char>(0xfd));
        for (int shift = 0; shift < 24; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    } else {
        out.push_back(static_cast<char>(0xfe));
        for (int shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    }
}

// A bounds-checked reader over one packet payload. Every accessor fails soft:
// it sets `ok = false` and returns an empty value, so a truncated or hostile
// packet becomes a protocol-error reason rather than an out-of-range read.
struct Reader {
    std::string_view data;
    std::size_t position = 0;
    bool ok = true;

    std::size_t remaining() const { return position <= data.size() ? data.size() - position : 0; }
    bool at_end() const { return remaining() == 0; }

    std::uint64_t fixed(std::size_t width) {
        if (remaining() < width) {
            ok = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(data[position + index]))
                     << (8 * index);
        }
        position += width;
        return value;
    }

    std::string_view bytes(std::size_t count) {
        if (remaining() < count) {
            ok = false;
            return {};
        }
        const std::string_view out = data.substr(position, count);
        position += count;
        return out;
    }

    std::string_view null_terminated() {
        const std::size_t end = data.find('\0', position);
        if (end == std::string_view::npos) {
            // Some servers omit the final NUL of the last field; take the rest.
            return rest();
        }
        const std::string_view out = data.substr(position, end - position);
        position = end + 1;
        return out;
    }

    std::string_view rest() {
        const std::string_view out = remaining() > 0 ? data.substr(position) : std::string_view{};
        position = data.size();
        return out;
    }

    // A length-encoded integer; `is_null` reports the 0xfb NULL marker used in
    // text result rows.
    std::uint64_t lenenc_int(bool* is_null = nullptr) {
        const std::uint64_t first = fixed(1);
        if (!ok) return 0;
        if (first < 0xfb) return first;
        if (first == 0xfb) {
            if (is_null != nullptr) {
                *is_null = true;
            } else {
                ok = false;
            }
            return 0;
        }
        if (first == 0xfc) return fixed(2);
        if (first == 0xfd) return fixed(3);
        if (first == 0xfe) return fixed(8);
        ok = false;  // 0xff is not a valid length prefix
        return 0;
    }

    std::string_view lenenc_string() {
        const std::uint64_t length = lenenc_int();
        if (!ok) return {};
        return bytes(static_cast<std::size_t>(length));
    }
};

// ERR_Packet → "code (SQLSTATE): message". The server's message names the user
// and host at most — never the password, which the server does not know.
std::string describe_error(std::string_view payload) {
    Reader reader{payload};
    reader.fixed(1);  // 0xff
    const std::uint64_t code = reader.fixed(2);
    std::string state;
    if (!reader.at_end() && reader.data[reader.position] == '#') {
        reader.fixed(1);
        state = std::string(reader.bytes(5));
    }
    std::string message(reader.rest());
    std::string out = "error " + std::to_string(code);
    if (!state.empty()) out += " (" + state + ")";
    if (!message.empty()) out += ": " + message;
    return out;
}

bool is_ok_packet(std::string_view payload) {
    return !payload.empty() && static_cast<unsigned char>(payload[0]) == 0x00;
}
bool is_err_packet(std::string_view payload) {
    return !payload.empty() && static_cast<unsigned char>(payload[0]) == 0xff;
}
bool is_eof_packet(std::string_view payload) {
    return !payload.empty() && static_cast<unsigned char>(payload[0]) == 0xfe &&
           payload.size() < 9;
}

bool is_valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xe0) == 0xc0 && lead >= 0xc2) {
            extra = 1;
        } else if ((lead & 0xf0) == 0xe0) {
            extra = 2;
        } else if ((lead & 0xf8) == 0xf0 && lead <= 0xf4) {
            extra = 3;
        } else {
            return false;
        }
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            if (index + offset >= text.size()) return false;
            if ((static_cast<unsigned char>(text[index + offset]) & 0xc0) != 0x80) {
                return false;
            }
        }
        index += extra + 1;
    }
    return true;
}

std::string to_hex_literal(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(2 + bytes.size() * 2);
    for (const char byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0f]);
    }
    return out;
}

bool is_byte_type(std::uint8_t type) {
    switch (type) {
        case kTypeTinyBlob: case kTypeMediumBlob: case kTypeLongBlob:
        case kTypeBlob: case kTypeVarString: case kTypeString:
        case kTypeVarchar: case kTypeGeometry:
            return true;
        default:
            return false;
    }
}

}  // namespace

// --- TLS mode -------------------------------------------------------------------

std::optional<MysqlTlsMode> parse_mysql_tls_mode(std::string_view sslmode) {
    std::string mode = to_lower(sslmode);
    std::ranges::replace(mode, '_', '-');
    if (mode.empty() || mode == "verify-full" || mode == "verify-identity") {
        return MysqlTlsMode::VerifyFull;
    }
    if (mode == "verify-ca") return MysqlTlsMode::VerifyCa;
    if (mode == "require" || mode == "required") return MysqlTlsMode::Require;
    if (mode == "disable" || mode == "disabled") return MysqlTlsMode::Disable;
    return std::nullopt;
}

std::string_view mysql_tls_mode_name(MysqlTlsMode mode) {
    switch (mode) {
        case MysqlTlsMode::Disable: return "disable";
        case MysqlTlsMode::Require: return "require";
        case MysqlTlsMode::VerifyCa: return "verify-ca";
        case MysqlTlsMode::VerifyFull: return "verify-full";
    }
    return "verify-full";
}

// --- Password plugins -------------------------------------------------------------

std::string mysql_native_password_response(std::string_view password,
                                           std::string_view nonce) {
    if (password.empty()) return {};
    const std::string stage1 = util::sha1_digest(password);
    const std::string stage2 = util::sha1_digest(stage1);
    std::string salted(nonce);
    salted += stage2;
    return xor_bytes(stage1, util::sha1_digest(salted));
}

std::string caching_sha2_password_response(std::string_view password,
                                           std::string_view nonce) {
    if (password.empty()) return {};
    const std::string stage1 = sha256_bytes(password);
    std::string salted = sha256_bytes(stage1);
    salted += nonce;
    return xor_bytes(stage1, sha256_bytes(salted));
}

bool is_tidb_version(std::string_view server_version) {
    std::string lowered(server_version);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lowered.find("tidb") != std::string::npos;
}

// --- The session ------------------------------------------------------------------

MysqlClientSession::MysqlClientSession(MysqlSessionOptions options)
    : options_(std::move(options)) {
    // The fencing runs before the caller's statement, in this order: every later
    // transaction in the session is read-only, the result is capped by the
    // server, and the statement runs inside an explicit read-only transaction
    // that is rolled back, never committed.
    commands_ = {
        "SET SESSION TRANSACTION READ ONLY",
        "SET SESSION sql_select_limit = " + std::to_string(options_.max_rows),
        "START TRANSACTION READ ONLY",
        options_.sql,
    };
}

std::string MysqlClientSession::frame(std::string_view payload) {
    // Every payload this client sends is far below the 16 MiB frame limit except
    // possibly the caller's statement; split it the way the protocol requires.
    std::string out;
    std::size_t offset = 0;
    for (;;) {
        const std::size_t chunk = std::min(payload.size() - offset, kFrameContinuation);
        ++sequence_;
        out.push_back(static_cast<char>(chunk & 0xff));
        out.push_back(static_cast<char>((chunk >> 8) & 0xff));
        out.push_back(static_cast<char>((chunk >> 16) & 0xff));
        out.push_back(static_cast<char>(sequence_));
        out.append(payload.substr(offset, chunk));
        offset += chunk;
        if (chunk < kFrameContinuation) break;
    }
    return out;
}

MysqlStep MysqlClientSession::fail(std::string reason) {
    phase_ = Phase::Done;
    result_.data = {};
    result_.error = "mysql: " + std::move(reason);
    return MysqlStep{MysqlStep::Action::Finished, {}};
}

MysqlStep MysqlClientSession::finish_with_quit() {
    phase_ = Phase::Done;
    sequence_ = 0xff;  // a command starts at sequence 0
    const std::string quit(1, static_cast<char>(kComQuit));
    return MysqlStep{MysqlStep::Action::Finished, frame(quit)};
}

MysqlStep MysqlClientSession::transport_failed(std::string reason) {
    if (phase_ == Phase::Done) return MysqlStep{MysqlStep::Action::Finished, {}};
    return fail(std::move(reason));
}

MysqlStep MysqlClientSession::receive(std::string_view bytes) {
    if (phase_ == Phase::Done) return MysqlStep{MysqlStep::Action::Finished, {}};
    buffer_.append(bytes);

    // Frame as many whole packets as arrived; each answer is written before the
    // next packet is looked at (the protocol is strictly request/response, so at
    // most one answer is produced per batch).
    MysqlStep step;
    std::string pending;
    for (;;) {
        if (buffer_.size() < 4) break;
        const std::size_t length =
            static_cast<unsigned char>(buffer_[0]) |
            (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[1])) << 8) |
            (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[2])) << 16);
        if (buffer_.size() < 4 + length) break;
        sequence_ = static_cast<std::uint8_t>(buffer_[3]);
        if (partial_payload_.size() + length > kMaxLogicalPacket) {
            return fail("the server sent a packet larger than " +
                        std::to_string(kMaxLogicalPacket) + " bytes");
        }
        partial_payload_.append(buffer_, 4, length);
        buffer_.erase(0, 4 + length);
        if (length == kFrameContinuation) continue;  // more of the same packet follows

        std::string payload = std::move(partial_payload_);
        partial_payload_.clear();
        step = handle_packet(payload);
        pending += step.send;
        if (step.action != MysqlStep::Action::Read) break;
    }
    step.send = std::move(pending);
    return step;
}

MysqlStep MysqlClientSession::handle_packet(std::string_view payload) {
    switch (phase_) {
        case Phase::Greeting: return on_greeting(payload);
        case Phase::Tls: return fail("the server sent data before the TLS handshake");
        case Phase::Auth: return on_auth(payload);
        case Phase::Command: return on_command_response(payload);
        case Phase::Columns: return on_column(payload);
        case Phase::Rows: return on_row(payload);
        case Phase::Rollback: return on_rollback(payload);
        case Phase::Done: break;
    }
    return MysqlStep{MysqlStep::Action::Finished, {}};
}

MysqlStep MysqlClientSession::on_greeting(std::string_view payload) {
    if (is_err_packet(payload)) {
        return fail("the server refused the connection: " + describe_error(payload));
    }
    Reader reader{payload};
    const std::uint64_t protocol = reader.fixed(1);
    if (!reader.ok || protocol != 10) {
        return fail("unsupported handshake protocol version " + std::to_string(protocol) +
                    " (only HandshakeV10, MySQL 4.1+, is spoken)");
    }
    server_version_ = std::string(reader.null_terminated());
    reader.fixed(4);  // connection id
    nonce_ = std::string(reader.bytes(8));
    reader.fixed(1);  // filler
    server_capabilities_ = static_cast<std::uint32_t>(reader.fixed(2));
    std::size_t auth_data_length = 0;
    if (!reader.at_end()) {
        reader.fixed(1);  // character set
        reader.fixed(2);  // status flags
        server_capabilities_ |= static_cast<std::uint32_t>(reader.fixed(2)) << 16;
        auth_data_length = static_cast<std::size_t>(reader.fixed(1));
        reader.bytes(10);  // reserved
        if ((server_capabilities_ & kClientSecureConnection) != 0) {
            const std::size_t part2 =
                std::max<std::size_t>(13, auth_data_length > 8 ? auth_data_length - 8 : 0);
            const std::string_view more = reader.bytes(part2);
            // The last byte of part 2 is a NUL terminator, not scramble.
            nonce_ += std::string(more.substr(0, more.empty() ? 0 : more.size() - 1));
        }
        if ((server_capabilities_ & kClientPluginAuth) != 0) {
            auth_plugin_ = std::string(reader.null_terminated());
        }
    }
    if (!reader.ok) return fail("the server's handshake packet is truncated");
    if ((server_capabilities_ & kClientProtocol41) == 0 ||
        (server_capabilities_ & kClientSecureConnection) == 0) {
        return fail("the server " + server_version_ +
                    " does not speak protocol 4.1 with secure authentication");
    }
    if (auth_plugin_.empty()) auth_plugin_ = "mysql_native_password";

    // TiDB (its greeting reads like `8.0.11-TiDB-v7.5.1`) speaks this protocol
    // but does not enforce READ ONLY: with tidb_enable_noop_functions OFF, its
    // default, `SET SESSION TRANSACTION READ ONLY` and `START TRANSACTION READ
    // ONLY` are errors, and with it ON they are accepted and ignored. So for
    // TiDB, whatever connector type reached it, the statement must hold no word
    // that writes (checked here, before any credential is sent), and it runs in
    // a plain transaction that is rolled back, never committed, so a write the
    // guard could not see is undone. `sql_select_limit` works as in MySQL.
    if (is_tidb_version(server_version_)) {
        tidb_ = true;
        if (std::string rejection = write_word_rejection(
                options_.sql, WriteWordDialect::Mysql, "a TiDB connection");
            !rejection.empty()) {
            return fail(rejection);
        }
        commands_ = {
            "SET SESSION sql_select_limit = " + std::to_string(options_.max_rows),
            "START TRANSACTION",
            options_.sql,
        };
    }

    client_capabilities_ = kClientLongPassword | kClientLongFlag | kClientProtocol41 |
                           kClientTransactions | kClientSecureConnection;
    if (!options_.database.empty()) client_capabilities_ |= kClientConnectWithDb;
    client_capabilities_ |=
        server_capabilities_ & (kClientPluginAuth | kClientPluginAuthLenencData);
    // Never offered, whatever the server supports: more than one statement per
    // query, and LOAD DATA LOCAL (a server asking the client for a local file).
    client_capabilities_ &= ~(kClientMultiStatements | kClientLocalFiles);

    if (options_.tls_mode == MysqlTlsMode::Disable) return send_handshake_response();

    if ((server_capabilities_ & kClientSsl) == 0) {
        // Refused before any credential is sent.
        return fail("the server " + server_version_ +
                    " does not offer TLS, and sslmode is " +
                    std::string(mysql_tls_mode_name(options_.tls_mode)) +
                    " (set sslmode=disable to connect in plaintext deliberately)");
    }
    client_capabilities_ |= kClientSsl;
    std::string request;
    put_u32(request, client_capabilities_);
    put_u32(request, kMaxPacketSize);
    request.push_back(static_cast<char>(kCharsetUtf8mb4));
    request.append(23, '\0');
    phase_ = Phase::Tls;
    return MysqlStep{MysqlStep::Action::StartTls, frame(request)};
}

MysqlStep MysqlClientSession::tls_established() {
    if (phase_ != Phase::Tls) return fail("a TLS handshake completed out of order");
    tls_active_ = true;
    return send_handshake_response();
}

std::string MysqlClientSession::auth_response_for(const std::string& plugin,
                                                  std::string& reason) const {
    if (plugin == "mysql_native_password") {
        return mysql_native_password_response(options_.password, nonce_.substr(0, 20));
    }
    if (plugin == "caching_sha2_password") {
        return caching_sha2_password_response(options_.password, nonce_.substr(0, 20));
    }
    if (plugin == "sha256_password" || plugin == "mysql_clear_password") {
        if (!tls_active_) {
            reason = "the account uses " + plugin +
                     ", which sends the password itself; that is only done over "
                     "TLS (use an sslmode other than disable)";
            return {};
        }
        return options_.password + std::string(1, '\0');
    }
    reason = "the server asked for the unsupported authentication plugin '" + plugin +
             "' (supported: caching_sha2_password, mysql_native_password, and "
             "sha256_password / mysql_clear_password over TLS)";
    return {};
}

MysqlStep MysqlClientSession::send_handshake_response() {
    std::string plugin = auth_plugin_;
    std::string reason;
    std::string response = auth_response_for(plugin, reason);
    if (!reason.empty()) {
        // An initial plugin we cannot answer: answer as mysql_native_password and
        // let the server switch us to what the account really uses — the
        // standard client behaviour. A plaintext-only plugin is never answered.
        if (plugin == "sha256_password" || plugin == "mysql_clear_password") {
            return fail(reason);
        }
        plugin = "mysql_native_password";
        response = auth_response_for(plugin, reason);
    }

    std::string packet;
    put_u32(packet, client_capabilities_);
    put_u32(packet, kMaxPacketSize);
    packet.push_back(static_cast<char>(kCharsetUtf8mb4));
    packet.append(23, '\0');
    packet += options_.user;
    packet.push_back('\0');
    if ((client_capabilities_ & kClientPluginAuthLenencData) != 0) {
        put_lenenc_int(packet, response.size());
    } else {
        packet.push_back(static_cast<char>(response.size()));
    }
    packet += response;
    if ((client_capabilities_ & kClientConnectWithDb) != 0) {
        packet += options_.database;
        packet.push_back('\0');
    }
    if ((client_capabilities_ & kClientPluginAuth) != 0) {
        packet += plugin;
        packet.push_back('\0');
    }
    auth_plugin_ = plugin;
    phase_ = Phase::Auth;
    return MysqlStep{MysqlStep::Action::Read, frame(packet)};
}

MysqlStep MysqlClientSession::on_auth(std::string_view payload) {
    if (payload.empty()) return fail("the server sent an empty authentication reply");
    const auto header = static_cast<unsigned char>(payload[0]);
    if (header == 0x00) return send_next_command();  // authenticated
    if (header == 0xff) {
        return fail("authentication failed for user '" + options_.user +
                    "': " + describe_error(payload));
    }
    if (header == 0xfe) {
        // AuthSwitchRequest: the account uses a different plugin.
        if (payload.size() == 1) {
            return fail("the server asked for the pre-4.1 password scheme, which is "
                        "not supported");
        }
        Reader reader{payload};
        reader.fixed(1);
        const std::string plugin(reader.null_terminated());
        std::string data(reader.rest());
        if (!data.empty() && data.back() == '\0') data.pop_back();
        nonce_ = data;
        std::string reason;
        const std::string response = auth_response_for(plugin, reason);
        if (!reason.empty()) return fail(reason);
        auth_plugin_ = plugin;
        return MysqlStep{MysqlStep::Action::Read, frame(response)};
    }
    if (header == 0x01) {
        // AuthMoreData — caching_sha2_password's fast/full authentication.
        if (auth_plugin_ != "caching_sha2_password" || payload.size() < 2) {
            return fail("the server sent " + auth_plugin_ +
                        " data this client does not handle (RSA public-key exchange "
                        "is not implemented; use TLS)");
        }
        const auto status = static_cast<unsigned char>(payload[1]);
        if (status == 0x03) return MysqlStep{MysqlStep::Action::Read, {}};  // fast auth; OK follows
        if (status == 0x04) {
            if (!tls_active_) {
                return fail(
                    "the server needs caching_sha2_password full authentication, "
                    "which sends the password and is only done over TLS (RSA "
                    "public-key exchange is not implemented); use an sslmode "
                    "other than disable");
            }
            return MysqlStep{MysqlStep::Action::Read,
                             frame(options_.password + std::string(1, '\0'))};
        }
        return fail("unexpected caching_sha2_password status " + std::to_string(status));
    }
    return fail("unexpected authentication reply (header byte " + std::to_string(header) + ")");
}

MysqlStep MysqlClientSession::send_next_command() {
    if (next_command_ >= commands_.size()) {
        // Every statement ran: roll the read-only transaction back.
        phase_ = Phase::Rollback;
        sequence_ = 0xff;
        std::string rollback(1, static_cast<char>(kComQuery));
        rollback += "ROLLBACK";
        return MysqlStep{MysqlStep::Action::Read, frame(rollback)};
    }
    phase_ = Phase::Command;
    sequence_ = 0xff;  // a command starts at sequence 0
    std::string packet(1, static_cast<char>(kComQuery));
    packet += commands_[next_command_++];
    return MysqlStep{MysqlStep::Action::Read, frame(packet)};
}

MysqlStep MysqlClientSession::on_command_response(std::string_view payload) {
    const bool is_query = next_command_ == commands_.size();
    if (payload.empty()) return fail("the server sent an empty reply");
    if (is_err_packet(payload)) {
        return fail(is_query ? "the query failed: " + describe_error(payload)
                             : "could not make the session read-only: " +
                                   describe_error(payload));
    }
    if (is_ok_packet(payload)) {
        // A fencing statement succeeded — or the query itself returned no
        // result set (zero columns, zero rows). Either way, on to the next.
        return send_next_command();
    }
    if (static_cast<unsigned char>(payload[0]) == 0xfb) {
        // LOCAL INFILE request. Never offered (no CLIENT_LOCAL_FILES), never honoured.
        return fail("the server asked to read a local file (LOAD DATA LOCAL); refused");
    }
    if (!is_query) {
        return fail("a session-fencing statement unexpectedly returned rows");
    }
    Reader reader{payload};
    const std::uint64_t count = reader.lenenc_int();
    if (!reader.ok || count == 0 || count > 4096) {
        return fail("malformed result-set header");
    }
    column_count_ = static_cast<std::size_t>(count);
    columns_.clear();
    columns_eof_seen_ = false;
    phase_ = Phase::Columns;
    return MysqlStep{MysqlStep::Action::Read, {}};
}

MysqlStep MysqlClientSession::on_column(std::string_view payload) {
    if (is_err_packet(payload)) return fail("the query failed: " + describe_error(payload));
    if (columns_.size() == column_count_) {
        // The EOF that closes the column definitions (CLIENT_DEPRECATE_EOF is
        // never offered, so it is always present).
        if (!is_eof_packet(payload)) return fail("missing EOF after the column definitions");
        result_.data.columns.clear();
        for (const Column& column : columns_) result_.data.columns.push_back(column.name);
        phase_ = Phase::Rows;
        return MysqlStep{MysqlStep::Action::Read, {}};
    }
    Reader reader{payload};
    reader.lenenc_string();  // catalog ("def")
    reader.lenenc_string();  // schema
    reader.lenenc_string();  // table
    reader.lenenc_string();  // org_table
    Column column;
    column.name = std::string(reader.lenenc_string());
    reader.lenenc_string();  // org_name
    reader.lenenc_int();     // length of the fixed fields (0x0c)
    column.charset = static_cast<std::uint16_t>(reader.fixed(2));
    reader.fixed(4);  // column length
    column.type = static_cast<std::uint8_t>(reader.fixed(1));
    column.flags = static_cast<std::uint16_t>(reader.fixed(2));
    if (!reader.ok) return fail("malformed column definition");
    columns_.push_back(std::move(column));
    return MysqlStep{MysqlStep::Action::Read, {}};
}

namespace {

// A text-protocol cell → a typed JSON value, so the landed dataset is directly
// featurizable: integers stay integers, decimals and floats are numbers, BIT is
// its integer value, binary bytes become a 0x… hex string (JSON text must be
// UTF-8), and everything else — dates, times, JSON, ENUM/SET — stays the
// server's own text. Anything that does not parse as its type stays a string:
// lossless, never a guess.
nlohmann::json coerce_cell(std::string_view text, std::uint8_t type, std::uint16_t charset,
                           std::uint16_t flags) {
    switch (type) {
        case kTypeTiny: case kTypeShort: case kTypeLong: case kTypeLongLong:
        case kTypeInt24: case kTypeYear: {
            if ((flags & kFlagUnsigned) != 0) {
                std::uint64_t value = 0;
                const auto [end, error] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                if (error == std::errc() && end == text.data() + text.size()) {
                    if (value <= static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max())) {
                        return static_cast<std::int64_t>(value);
                    }
                    return value;
                }
            } else {
                std::int64_t value = 0;
                const auto [end, error] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                if (error == std::errc() && end == text.data() + text.size()) return value;
            }
            return std::string(text);
        }
        case kTypeFloat: case kTypeDouble: case kTypeDecimal: case kTypeNewDecimal: {
            double value = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error == std::errc() && end == text.data() + text.size()) return value;
            return std::string(text);
        }
        case kTypeBit: {
            if (text.size() > 8) return to_hex_literal(text);
            std::uint64_t value = 0;
            for (const char byte : text) value = (value << 8) | static_cast<unsigned char>(byte);
            return static_cast<std::int64_t>(value);
        }
        default:
            break;
    }
    if ((charset == kCharsetBinary && is_byte_type(type)) || !is_valid_utf8(text)) {
        return to_hex_literal(text);
    }
    return std::string(text);
}

}  // namespace

MysqlStep MysqlClientSession::on_row(std::string_view payload) {
    if (is_err_packet(payload)) return fail("the query failed: " + describe_error(payload));
    if (is_eof_packet(payload)) return send_next_command();  // → ROLLBACK
    Reader reader{payload};
    nlohmann::json row = nlohmann::json::object();
    for (const Column& column : columns_) {
        bool is_null = false;
        const std::uint64_t length = reader.lenenc_int(&is_null);
        if (!reader.ok) return fail("malformed result row");
        if (is_null) {
            row[column.name] = nullptr;
            continue;
        }
        const std::string_view text = reader.bytes(static_cast<std::size_t>(length));
        if (!reader.ok) return fail("malformed result row");
        row[column.name] = coerce_cell(text, column.type, column.charset, column.flags);
    }
    result_.data.rows.push_back(std::move(row));
    if (result_.data.rows.size() >= options_.max_rows) {
        // The cap is reached: stop here, mid-result. Nothing more is read; the
        // driver closes the connection, which ends the read-only transaction on
        // the server (nothing was ever committed).
        phase_ = Phase::Done;
        return MysqlStep{MysqlStep::Action::Finished, {}};
    }
    return MysqlStep{MysqlStep::Action::Read, {}};
}

MysqlStep MysqlClientSession::on_rollback(std::string_view payload) {
    if (is_err_packet(payload)) {
        return fail("could not roll back the read-only transaction: " +
                    describe_error(payload));
    }
    return finish_with_quit();
}

// --- The injection point ------------------------------------------------------------

namespace {
MysqlQueryRunner& runner_slot() {
    static MysqlQueryRunner runner;  // empty until installed
    return runner;
}
}  // namespace

void install_mysql_query_runner(MysqlQueryRunner runner) {
    runner_slot() = std::move(runner);
}

bool has_mysql_query_runner() { return static_cast<bool>(runner_slot()); }

const MysqlQueryRunner& mysql_query_runner() { return runner_slot(); }

}  // namespace zetta::connect
