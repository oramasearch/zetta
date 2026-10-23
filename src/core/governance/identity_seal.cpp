#include "zetta/governance/identity_seal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "../catalog.hpp"

// The identity root's crypto adapter: Ed25519 sign/verify over identity
// records, and the root's on-disk pin. See docs/governance/identity-seal.md.
namespace zetta::governance {
namespace {

namespace fs = std::filesystem;

using EvpKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kSignatureBytes = 64;
constexpr std::string_view kRootDomain = "zetta-identity-root-v1";
constexpr std::string_view kSealDomain = "zetta-identity-seal-v2";
constexpr std::string_view kIdDomain = "zetta-identity-id-v1";

void set_reason(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

std::string to_hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(kDigits[bytes[i] >> 4]);
        hex.push_back(kDigits[bytes[i] & 0x0f]);
    }
    return hex;
}

template <std::size_t N>
std::optional<std::array<unsigned char, N>> from_hex(std::string_view hex) {
    if (hex.size() != N * 2) return std::nullopt;
    for (const char c : hex) {
        if (!is_lower_hex(c)) return std::nullopt;
    }
    auto nibble = [](char c) -> unsigned char {
        return c <= '9' ? static_cast<unsigned char>(c - '0')
                        : static_cast<unsigned char>(c - 'a' + 10);
    };
    std::array<unsigned char, N> bytes{};
    for (std::size_t i = 0; i < N; ++i) {
        bytes[i] = static_cast<unsigned char>((nibble(hex[i * 2]) << 4) |
                                              nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

std::array<unsigned char, 32> sha256(std::string_view input) {
    std::array<unsigned char, 32> digest{};
    unsigned int written = 0;
    EVP_Digest(input.data(), input.size(), digest.data(), &written, EVP_sha256(),
               nullptr);
    return digest;
}

// The bytes a seal signs: the domain, the root's public key, the catalogue key,
// and the value without its seal (its version and any tombstone marker
// included). nlohmann::json orders object keys, so every node that holds the
// same value builds the same bytes.
std::string seal_message(std::string_view root_public_key, std::string_view key,
                         const nlohmann::json& unsealed) {
    std::string message(kSealDomain);
    message += '\n';
    message += root_public_key;
    message += '\n';
    message += key;
    message += '\n';
    message += unsealed.dump();
    return message;
}

fs::path root_path(const std::string& data_dir) {
    return fs::path(data_dir) / "security" / "identity-root.json";
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool write_file(const fs::path& path, const std::string& contents,
                bool is_private) {
    // The mode is set before any byte is written, so a file holding the root's
    // seed is never readable by others, not even briefly (#1413).
    const mode_t mode = is_private ? (S_IRUSR | S_IWUSR)
                                   : (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, mode) == 0;  // an existing file keeps its old mode otherwise
    std::size_t written = 0;
    while (ok && written < contents.size()) {
        const ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    ok = (::close(fd) == 0) && ok;
    return ok;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& root_validator() {
    static nlohmann::json_schema::json_validator validator(identity_root_schema());
    return validator;
}

struct RootFile {
    std::string public_key;
    std::string seed;  // only for a generated root
};

std::optional<RootFile> parse_root_file(const std::string& contents,
                                        std::string* reason) {
    const nlohmann::json document =
        nlohmann::json::parse(contents, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_reason(reason, "identity-root.json: not valid JSON");
        return std::nullopt;
    }
    collecting_error_handler errors;
    root_validator().validate(document, errors);
    if (errors.failed) {
        set_reason(reason, "identity-root.json: " + errors.reason);
        return std::nullopt;
    }
    RootFile file;
    file.public_key = document["public_key"].get<std::string>();
    file.seed = document.value("seed", std::string{});
    return file;
}

bool write_root_file(const fs::path& path, const IdentityAuthority& root,
                     bool keep_seed) {
    nlohmann::json document{{"version", 1},
                            {"public_key", root.public_key_hex()}};
    if (keep_seed) document["seed"] = root.seed_hex();
    return write_file(path, document.dump(2) + "\n", /*is_private=*/keep_seed);
}

}  // namespace

bool is_identity_key(std::string_view key) {
    return key.starts_with(catalog::kUserPrefix) ||
           key.starts_with(catalog::kRolePrefix) ||
           key.starts_with(catalog::kTokenPrefix) ||
           key.starts_with(catalog::kUserRolePrefix) ||
           key.starts_with(catalog::kPolicyPrefix) ||
           key.starts_with(catalog::kPrincipalPrefix) ||
           key.starts_with(catalog::kAccessKeyPrefix);
}

std::optional<IdentityAuthority> IdentityAuthority::from_seed(
    const std::array<unsigned char, 32>& seed) {
    EvpKeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               seed.data(), seed.size()),
                  &EVP_PKEY_free);
    if (!key) return std::nullopt;
    IdentityAuthority root;
    std::size_t length = root.public_key_.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), root.public_key_.data(),
                                    &length) != 1 ||
        length != kKeyBytes) {
        return std::nullopt;
    }
    root.has_seed_ = true;
    root.seed_ = seed;
    root.public_key_hex_ = to_hex(root.public_key_.data(), root.public_key_.size());
    return root;
}

std::optional<IdentityAuthority> IdentityAuthority::from_owner_secret(
    std::string_view owner_secret) {
    if (owner_secret.empty()) return std::nullopt;
    std::string input(kRootDomain);
    input += '\n';
    input += owner_secret;
    return from_seed(sha256(input));
}

std::optional<IdentityAuthority> IdentityAuthority::generate() {
    std::array<unsigned char, 32> seed{};
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
        return std::nullopt;
    }
    return from_seed(seed);
}

std::optional<IdentityAuthority> IdentityAuthority::from_seed_hex(
    std::string_view seed_hex) {
    const auto seed = from_hex<kKeyBytes>(seed_hex);
    if (!seed) return std::nullopt;
    return from_seed(*seed);
}

std::optional<IdentityAuthority> IdentityAuthority::verify_only(
    std::string_view public_key_hex) {
    const auto public_key = from_hex<kKeyBytes>(public_key_hex);
    if (!public_key) return std::nullopt;
    EvpKeyPtr key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              public_key->data(),
                                              public_key->size()),
                  &EVP_PKEY_free);
    if (!key) return std::nullopt;
    IdentityAuthority root;
    root.public_key_ = *public_key;
    root.public_key_hex_ = std::string(public_key_hex);
    return root;
}

std::string IdentityAuthority::seed_hex() const {
    if (!has_seed_) return {};
    return to_hex(seed_.data(), seed_.size());
}

std::optional<nlohmann::json> IdentityAuthority::sign(std::string_view key,
                                                      nlohmann::json value) const {
    EvpKeyPtr private_key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                       seed_.data(), seed_.size()),
                          &EVP_PKEY_free);
    EvpMdCtxPtr context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!private_key || !context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                           private_key.get()) != 1) {
        return std::nullopt;
    }
    const std::string message = seal_message(public_key_hex_, key, value);
    std::array<unsigned char, kSignatureBytes> signature{};
    std::size_t length = signature.size();
    if (EVP_DigestSign(context.get(), signature.data(), &length,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1 ||
        length != kSignatureBytes) {
        return std::nullopt;
    }
    value[std::string(kIdentitySealField)] =
        std::string(kIdentitySealPrefix) + to_hex(signature.data(), length);
    return value;
}

std::optional<nlohmann::json> IdentityAuthority::seal(std::string_view key,
                                                      nlohmann::json value,
                                                      std::uint64_t version) const {
    if (!has_seed_ || !value.is_object() || version == 0) return std::nullopt;
    // A live record never carries the removal marker: a removal is sealed only
    // through seal_removal, so no caller turns a record into a tombstone.
    if (value.contains(std::string(kIdentityTombstoneField))) return std::nullopt;
    value.erase(std::string(kIdentitySealField));
    value[std::string(kIdentityVersionField)] = version;
    return sign(key, std::move(value));
}

std::optional<nlohmann::json> IdentityAuthority::seal_removal(
    std::string_view key, std::uint64_t version) const {
    if (!has_seed_ || version == 0) return std::nullopt;
    return sign(key, nlohmann::json{{std::string(kIdentityTombstoneField), true},
                                    {std::string(kIdentityVersionField), version}});
}

std::optional<OpenedIdentityRecord> IdentityAuthority::open(
    std::string_view key, const nlohmann::json& value) const {
    if (!value.is_object()) return std::nullopt;
    const auto found = value.find(std::string(kIdentitySealField));
    if (found == value.end() || !found->is_string()) return std::nullopt;
    const std::string& seal_text = found->get_ref<const std::string&>();
    if (!seal_text.starts_with(kIdentitySealPrefix)) return std::nullopt;
    const auto signature = from_hex<kSignatureBytes>(
        std::string_view(seal_text).substr(kIdentitySealPrefix.size()));
    if (!signature) return std::nullopt;
    // The version must be a positive integer: a record with none is from before
    // versions and cannot be ordered against a later one (#1341).
    const auto version = value.find(std::string(kIdentityVersionField));
    if (version == value.end() || !version->is_number_unsigned() ||
        version->get<std::uint64_t>() == 0) {
        return std::nullopt;
    }
    nlohmann::json unsealed = value;
    unsealed.erase(std::string(kIdentitySealField));
    EvpKeyPtr public_key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                     public_key_.data(),
                                                     public_key_.size()),
                         &EVP_PKEY_free);
    EvpMdCtxPtr context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!public_key || !context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                             public_key.get()) != 1) {
        return std::nullopt;
    }
    const std::string message = seal_message(public_key_hex_, key, unsealed);
    if (EVP_DigestVerify(context.get(), signature->data(), signature->size(),
                         reinterpret_cast<const unsigned char*>(message.data()),
                         message.size()) != 1) {
        return std::nullopt;
    }
    OpenedIdentityRecord record;
    record.version = version->get<std::uint64_t>();
    record.seal = seal_text;
    if (const auto marker = unsealed.find(std::string(kIdentityTombstoneField));
        marker != unsealed.end()) {
        // A sealed removal is the marker and the version, nothing else.
        if (!marker->is_boolean() || !marker->get<bool>() || unsealed.size() != 2) {
            return std::nullopt;
        }
        record.removed = true;
        record.value = nlohmann::json::object();
        return record;
    }
    unsealed.erase(std::string(kIdentityVersionField));
    record.value = std::move(unsealed);
    return record;
}

bool identity_record_supersedes(const OpenedIdentityRecord& a,
                                const OpenedIdentityRecord& b) {
    if (a.version != b.version) return a.version > b.version;
    return a.seal > b.seal;
}

std::uint64_t next_identity_version(const IdentityAuthority& root,
                                    std::string_view key,
                                    const nlohmann::json* current,
                                    std::uint64_t floor) {
    using namespace std::chrono;
    std::uint64_t version = static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    if (floor >= version) version = floor + 1;
    if (current) {
        if (const auto opened = root.open(key, *current);
            opened && opened->version >= version) {
            version = opened->version + 1;
        }
    }
    return version;
}

std::string IdentityAuthority::derived_id(std::string_view purpose) const {
    std::string input(kIdDomain);
    input += '\n';
    input += public_key_hex_;
    input += '\n';
    input += purpose;
    std::array<unsigned char, 32> digest = sha256(input);
    // RFC 9562 layout: version 8 (custom, name-based) and the RFC variant, so the
    // id is a well-formed UUID that can never collide with a random v4 one.
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0f) | 0x80);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3f) | 0x80);
    const std::string hex = to_hex(digest.data(), 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
           "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

IdentityRootSetup load_or_create_identity_root(const std::string& data_dir,
                                               const std::string& owner_secret,
                                               const std::string& root_override,
                                               std::string* reason) {
    IdentityRootSetup setup;
    std::optional<IdentityAuthority> override_root;
    if (!root_override.empty()) {
        override_root = IdentityAuthority::verify_only(root_override);
        if (!override_root) {
            set_reason(reason,
                       "--identity-root must be 64 lowercase hex characters (an "
                       "Ed25519 public key)");
            return setup;
        }
    }
    std::optional<IdentityAuthority> owner_root;
    if (!owner_secret.empty()) {
        owner_root = IdentityAuthority::from_owner_secret(owner_secret);
        if (!owner_root) {
            set_reason(reason, "failed to derive the identity root from the admin token");
            return setup;
        }
    }

    const fs::path path = root_path(data_dir);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        set_reason(reason, "cannot create " + path.parent_path().string() + ": " +
                               ec.message());
        return setup;
    }

    // A pinned root wins: the first start decided which root this node trusts,
    // and no later flag or token silently moves it.
    if (const std::optional<std::string> stored = read_file(path)) {
        const std::optional<RootFile> file = parse_root_file(*stored, reason);
        if (!file) return setup;  // malformed: fail, never clobber
        if (override_root && override_root->public_key_hex() != file->public_key) {
            set_reason(reason,
                       "--identity-root " + root_override +
                           " differs from the root this node pinned (" +
                           file->public_key + ", in " + path.string() + ")");
            return setup;
        }
        if (!file->seed.empty()) {
            auto generated = IdentityAuthority::from_seed_hex(file->seed);
            if (!generated || generated->public_key_hex() != file->public_key) {
                set_reason(reason, "identity-root.json: the seed does not match "
                                   "the public key");
                return setup;
            }
            setup.authority = std::move(generated);
        } else if (owner_root && owner_root->public_key_hex() == file->public_key) {
            // Pinned by a build that stored only the public key: store the seed
            // now, while the token still derives it, so a later token rotation
            // does not leave this node verify-only.
            setup.authority = std::move(owner_root);
            if (!write_root_file(path, *setup.authority, /*keep_seed=*/true)) {
                setup.warning = "cannot store the identity root's seed in " +
                                path.string() +
                                ": a later admin-token rotation will leave this node "
                                "verify-only";
            }
        } else {
            setup.authority = IdentityAuthority::verify_only(file->public_key);
            if (owner_root) {
                setup.warning =
                    "the admin token does not derive this node's pinned identity "
                    "root " + file->public_key +
                    ": the node verifies identity but issues none, and seeds no owner";
            }
        }
        if (!setup.authority) {
            set_reason(reason, "identity-root.json: unusable public key");
        }
        return setup;
    }

    // First start: pin the root. A root this node can seal with keeps its seed in
    // the file (mode 0600), whether derived from the admin token or generated, so
    // the root outlives a rotation of the token it was derived from.
    if (override_root) {
        if (owner_root && owner_root->public_key_hex() == override_root->public_key_hex()) {
            setup.authority = std::move(owner_root);
        } else {
            setup.authority = std::move(override_root);
            if (owner_root) {
                setup.warning =
                    "the admin token does not derive --identity-root: the node "
                    "verifies identity but issues none, and seeds no owner";
            }
        }
    } else if (owner_root) {
        setup.authority = std::move(owner_root);
    } else {
        setup.authority = IdentityAuthority::generate();
        if (!setup.authority) {
            set_reason(reason, "failed to generate an identity root");
            return setup;
        }
    }
    if (!write_root_file(path, *setup.authority, setup.authority->can_seal())) {
        set_reason(reason, "cannot write " + path.string());
        setup.authority.reset();
        return setup;
    }
    setup.pinned_now = true;
    return setup;
}

const nlohmann::json& identity_root_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.IdentityRoot",
      "type": "object",
      "properties": {
        "version":    { "type": "integer", "const": 1 },
        "public_key": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
        "seed":       { "type": "string", "pattern": "^[0-9a-f]{64}$" }
      },
      "required": ["version", "public_key"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
