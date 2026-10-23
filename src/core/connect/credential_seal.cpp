#include "zetta/connect/credential_seal.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>

#include "../util/private_directory.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace zetta::connect {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kRootSecretBytes = 32;
constexpr std::string_view kAlgorithm = "AES-256-GCM";
// Domain separation: the join token also seeds the cluster CA (security.cpp);
// this salt and info make the sealing key an unrelated value.
constexpr std::string_view kHkdfSalt = "zetta.connect.credential-key";
constexpr std::string_view kHkdfInfo = "v1";
constexpr std::string_view kAadPrefix = "zetta.connect.connection:";
constexpr const char* kLocalRootSecretFile = "connection-root-secret";

void set_reason(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

std::string to_hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0f]);
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::optional<std::vector<unsigned char>> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return out;
}

bool is_lower_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return hex_value(c) >= 0; });
}

// HKDF-SHA256(root secret) -> the 256-bit sealing key.
std::optional<CredentialKey> derive_key(const std::string& root_secret) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
    CredentialKey key;
    std::size_t length = key.bytes.size();
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            context.get(), reinterpret_cast<const unsigned char*>(kHkdfSalt.data()),
            static_cast<int>(kHkdfSalt.size())) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            context.get(), reinterpret_cast<const unsigned char*>(root_secret.data()),
            static_cast<int>(root_secret.size())) <= 0 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            context.get(), reinterpret_cast<const unsigned char*>(kHkdfInfo.data()),
            static_cast<int>(kHkdfInfo.size())) <= 0 ||
        EVP_PKEY_derive(context.get(), key.bytes.data(), &length) <= 0 ||
        length != key.bytes.size()) {
        return std::nullopt;
    }
    // The id is public: a hash of the key, never the key.
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int written = 0;
    const std::string labelled = std::string("zetta.connect.key-id:") +
        std::string(reinterpret_cast<const char*>(key.bytes.data()), key.bytes.size());
    if (EVP_Digest(labelled.data(), labelled.size(), digest, &written, EVP_sha256(),
                   nullptr) != 1 ||
        written < 8) {
        return std::nullopt;
    }
    key.id = to_hex(digest, 8);
    return key;
}

std::string aad_for(std::string_view context) {
    return std::string(kAadPrefix) + std::string(context);
}

}  // namespace

std::optional<CredentialKeyring> CredentialKeyring::from_root_secrets(
    const std::string& current, const std::vector<std::string>& retired,
    std::string* reason) {
    if (current.empty()) {
        set_reason(reason, "credential keyring: no root secret");
        return std::nullopt;
    }
    CredentialKeyring keyring;
    keyring.current_ = derive_key(current);
    if (!keyring.current_) {
        set_reason(reason, "credential keyring: key derivation failed");
        return std::nullopt;
    }
    for (const std::string& secret : retired) {
        if (secret.empty() || secret == current) continue;
        std::optional<CredentialKey> key = derive_key(secret);
        if (!key) {
            set_reason(reason, "credential keyring: key derivation failed");
            return std::nullopt;
        }
        keyring.retired_.push_back(std::move(*key));
    }
    return keyring;
}

const CredentialKey* CredentialKeyring::current() const {
    return current_ ? &*current_ : nullptr;
}

const CredentialKey* CredentialKeyring::find(std::string_view id) const {
    if (current_ && current_->id == id) return &*current_;
    for (const CredentialKey& key : retired_) {
        if (key.id == id) return &key;
    }
    return nullptr;
}

std::optional<CredentialKeyring> load_or_create_local_keyring(
    const std::string& data_dir, std::string* reason) {
    const fs::path directory = fs::path(data_dir) / "security";
    const auto private_directory = util::PrivateDirectory::open(directory, reason);
    if (!private_directory) return std::nullopt;
    const fs::path path = directory / kLocalRootSecretFile;
    std::string secret;
    std::optional<std::string> stored;
    if (!private_directory->read(kLocalRootSecretFile, stored, reason)) return std::nullopt;
    if (stored) {
        secret = *stored;
        while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) {
            secret.pop_back();
        }
        if (secret.size() != kRootSecretBytes * 2 || !is_lower_hex(secret)) {
            // Fail rather than overwrite: replacing it would orphan every
            // credential sealed under it.
            set_reason(reason, path.string() + ": not a 64-character hex secret");
            return std::nullopt;
        }
    } else {
        std::array<unsigned char, kRootSecretBytes> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            set_reason(reason, "CSPRNG failure generating the connection root secret");
            return std::nullopt;
        }
        secret = to_hex(bytes.data(), bytes.size());
        if (!private_directory->write(kLocalRootSecretFile, secret + "\n", reason)) {
            return std::nullopt;
        }
    }
    return CredentialKeyring::from_root_secrets(secret, {}, reason);
}

std::optional<nlohmann::json> seal_credential(const CredentialKeyring& keyring,
                                              std::string_view plaintext,
                                              std::string_view context,
                                              std::string* reason) {
    const CredentialKey* key = keyring.current();
    if (!key) {
        set_reason(reason, "credential seal: this node holds no sealing key");
        return std::nullopt;
    }
    std::array<unsigned char, kNonceBytes> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        set_reason(reason, "credential seal: CSPRNG failure");
        return std::nullopt;
    }
    const std::string aad = aad_for(context);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    std::vector<unsigned char> ciphertext(plaintext.size() + kTagBytes);
    std::array<unsigned char, kTagBytes> tag{};
    int written = 0;
    int total = 0;
    if (!cipher ||
        EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                           nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(cipher.get(), nullptr, nullptr, key->bytes.data(),
                           nonce.data()) != 1 ||
        EVP_EncryptUpdate(cipher.get(), nullptr, &written,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) != 1 ||
        EVP_EncryptUpdate(cipher.get(), ciphertext.data(), &written,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        set_reason(reason, "credential seal: encryption failed");
        return std::nullopt;
    }
    total = written;
    if (EVP_EncryptFinal_ex(cipher.get(), ciphertext.data() + total, &written) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) != 1) {
        set_reason(reason, "credential seal: encryption failed");
        return std::nullopt;
    }
    total += written;
    nlohmann::json sealed;
    sealed["v"] = 1;
    sealed["alg"] = std::string(kAlgorithm);
    sealed["kid"] = key->id;
    sealed["nonce"] = to_hex(nonce.data(), nonce.size());
    sealed["ciphertext"] = to_hex(ciphertext.data(), static_cast<std::size_t>(total));
    sealed["tag"] = to_hex(tag.data(), tag.size());
    return sealed;
}

bool is_sealed_credential(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 6) return false;
    const auto text = [&](const char* field) -> const std::string* {
        const auto found = value.find(field);
        if (found == value.end() || !found->is_string()) return nullptr;
        return found->get_ptr<const std::string*>();
    };
    const auto version = value.find("v");
    if (version == value.end() || !version->is_number_integer() || *version != 1) {
        return false;
    }
    const std::string* alg = text("alg");
    const std::string* kid = text("kid");
    const std::string* nonce = text("nonce");
    const std::string* ciphertext = text("ciphertext");
    const std::string* tag = text("tag");
    return alg && *alg == kAlgorithm && kid && kid->size() == 16 &&
           is_lower_hex(*kid) && nonce && nonce->size() == kNonceBytes * 2 &&
           is_lower_hex(*nonce) && ciphertext && ciphertext->size() % 2 == 0 &&
           is_lower_hex(*ciphertext) && tag && tag->size() == kTagBytes * 2 &&
           is_lower_hex(*tag);
}

std::string sealed_key_id(const nlohmann::json& sealed) {
    if (!is_sealed_credential(sealed)) return {};
    return sealed.at("kid").get<std::string>();
}

std::optional<std::string> unseal_credential(const CredentialKeyring& keyring,
                                             const nlohmann::json& sealed,
                                             std::string_view context,
                                             std::string* reason) {
    if (!is_sealed_credential(sealed)) {
        set_reason(reason, "credential unseal: not a sealed credential");
        return std::nullopt;
    }
    const std::string kid = sealed.at("kid").get<std::string>();
    const CredentialKey* key = keyring.find(kid);
    if (!key) {
        set_reason(reason, "credential unseal: sealed under key " + kid +
                               ", which this node does not hold");
        return std::nullopt;
    }
    const auto nonce = from_hex(sealed.at("nonce").get<std::string>());
    const auto ciphertext = from_hex(sealed.at("ciphertext").get<std::string>());
    auto tag = from_hex(sealed.at("tag").get<std::string>());
    if (!nonce || !ciphertext || !tag) {
        set_reason(reason, "credential unseal: malformed hex");
        return std::nullopt;
    }
    const std::string aad = aad_for(context);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
        EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    std::vector<unsigned char> plaintext(ciphertext->size() + kTagBytes);
    int written = 0;
    int total = 0;
    if (!cipher ||
        EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                           nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce->size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr, key->bytes.data(),
                           nonce->data()) != 1 ||
        EVP_DecryptUpdate(cipher.get(), nullptr, &written,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) != 1 ||
        EVP_DecryptUpdate(cipher.get(), plaintext.data(), &written, ciphertext->data(),
                          static_cast<int>(ciphertext->size())) != 1) {
        set_reason(reason, "credential unseal: decryption failed");
        return std::nullopt;
    }
    total = written;
    if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(tag->size()), tag->data()) != 1 ||
        EVP_DecryptFinal_ex(cipher.get(), plaintext.data() + total, &written) != 1) {
        set_reason(reason, "credential unseal: authentication failed (tampered, or "
                           "sealed for another connection)");
        return std::nullopt;
    }
    total += written;
    return std::string(reinterpret_cast<const char*>(plaintext.data()),
                       static_cast<std::size_t>(total));
}

const nlohmann::json& sealed_credential_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.SealedCredential",
      "description": "A connector credential sealed with AES-256-GCM under a key derived from a root secret held outside the replicated catalogue (#1230). The connection name is the associated data.",
      "type": "object",
      "properties": {
        "v":          { "type": "integer", "const": 1 },
        "alg":        { "type": "string", "const": "AES-256-GCM" },
        "kid":        { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "nonce":      { "type": "string", "pattern": "^[0-9a-f]{24}$" },
        "ciphertext": { "type": "string", "pattern": "^([0-9a-f]{2})*$" },
        "tag":        { "type": "string", "pattern": "^[0-9a-f]{32}$" }
      },
      "required": ["v", "alg", "kid", "nonce", "ciphertext", "tag"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

}  // namespace zetta::connect
