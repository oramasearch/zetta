#include "zetta/cluster/security.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "../util/private_directory.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

// src/core/cluster/security.cpp is the crypto adapter: the one .cpp allowed to
// use OpenSSL libcrypto directly (key generation, X509 minting, PEM). The rest
// of zetta-core sees only the ClusterSecurity value type; the TLS *transport*
// (openssl_stream) lives behind zetta::runtime::net in cluster.cpp.
namespace zetta::cluster {
namespace {

// RAII for the OpenSSL C-API handles this file touches.
using EvpKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

constexpr std::size_t kTokenBytes = 32;
constexpr const char* kCaCommonName = "zetta-cluster-ca";
// Fixed validity window so the derived CA is identical on every node and no
// certificate expires under a running cluster (clock skew tolerant).
constexpr const char* kNotBefore = "20260101000000Z";
constexpr const char* kNotAfter = "20760101000000Z";

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

std::array<unsigned char, kTokenBytes> decode_token(std::string_view token) {
    auto nibble = [](char c) -> unsigned char {
        return c <= '9' ? static_cast<unsigned char>(c - '0')
                        : static_cast<unsigned char>(c - 'a' + 10);
    };
    std::array<unsigned char, kTokenBytes> bytes{};
    for (std::size_t i = 0; i < kTokenBytes; ++i) {
        bytes[i] = static_cast<unsigned char>(
            (nibble(token[i * 2]) << 4) | nibble(token[i * 2 + 1]));
    }
    return bytes;
}

// SHA-256 of `input` — the CA-seed KDF step (domain-separated below).
std::array<unsigned char, 32> sha256_bytes(std::string_view input) {
    std::array<unsigned char, 32> digest{};
    unsigned int written = 0;
    EVP_Digest(input.data(), input.size(), digest.data(), &written,
               EVP_sha256(), nullptr);
    return digest;
}

// The cluster CA keypair, derived deterministically from the token: Ed25519's
// private key IS a 32-byte seed, so hashing the token (domain-separated) gives
// every token holder the same CA key — no enrollment round-trip needed.
EvpKeyPtr derive_ca_key(std::string_view token) {
    const auto raw = decode_token(token);
    std::string seed_input(reinterpret_cast<const char*>(raw.data()),
                           raw.size());
    seed_input += "zetta-cluster-ca-v1";
    const auto seed = sha256_bytes(seed_input);
    return EvpKeyPtr(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                     seed.size()),
        &EVP_PKEY_free);
}

EvpKeyPtr generate_node_key() {
    EVP_PKEY* key = nullptr;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
        EVP_PKEY_keygen(ctx.get(), &key) <= 0) {
        return EvpKeyPtr(nullptr, &EVP_PKEY_free);
    }
    return EvpKeyPtr(key, &EVP_PKEY_free);
}

bool set_common_name(X509_NAME* name, const std::string& common_name) {
    return X509_NAME_add_entry_by_txt(
               name, "CN", MBSTRING_ASC,
               reinterpret_cast<const unsigned char*>(common_name.c_str()),
               -1, -1, 0) == 1;
}

bool add_extension(X509* certificate, X509* issuer, int nid,
                   const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension =
        X509V3_EXT_conf_nid(nullptr, &context, nid, value);
    if (!extension) return false;
    const bool added = X509_add_ext(certificate, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return added;
}

// Mint one certificate: `subject_cn` keyed by `subject_key`, signed by
// `issuer_key` under `issuer_cn`. `is_ca` selects the CA vs leaf extension
// profile. All fields are fixed (validity window above, serial from the
// subject CN) so the self-issued CA comes out identical on every node.
X509Ptr make_certificate(const std::string& subject_cn, EVP_PKEY* subject_key,
                         const std::string& issuer_cn, EVP_PKEY* issuer_key,
                         bool is_ca) {
    X509Ptr certificate(X509_new(), &X509_free);
    if (!certificate) return X509Ptr(nullptr, &X509_free);
    if (X509_set_version(certificate.get(), 2) != 1) {  // X509v3
        return X509Ptr(nullptr, &X509_free);
    }

    // Serial: first 8 bytes of SHA-256(subject CN), high bit cleared so it is
    // positive — deterministic, distinct per subject.
    const auto serial_digest = sha256_bytes(subject_cn);
    std::uint64_t serial = 0;
    for (int i = 0; i < 8; ++i) serial = (serial << 8) | serial_digest[i];
    serial &= ~(std::uint64_t{1} << 63);
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(certificate.get()), serial);

    if (!ASN1_TIME_set_string_X509(X509_getm_notBefore(certificate.get()),
                                   kNotBefore) ||
        !ASN1_TIME_set_string_X509(X509_getm_notAfter(certificate.get()),
                                   kNotAfter)) {
        return X509Ptr(nullptr, &X509_free);
    }

    if (!set_common_name(X509_get_subject_name(certificate.get()),
                         subject_cn) ||
        !set_common_name(X509_get_issuer_name(certificate.get()), issuer_cn) ||
        X509_set_pubkey(certificate.get(), subject_key) != 1) {
        return X509Ptr(nullptr, &X509_free);
    }

    const bool extensions_ok =
        is_ca ? add_extension(certificate.get(), certificate.get(),
                              NID_basic_constraints, "critical,CA:TRUE") &&
                    add_extension(certificate.get(), certificate.get(),
                                  NID_key_usage, "critical,keyCertSign,cRLSign")
              : add_extension(certificate.get(), nullptr,
                              NID_basic_constraints, "critical,CA:FALSE") &&
                    add_extension(certificate.get(), nullptr, NID_key_usage,
                                  "critical,digitalSignature") &&
                    add_extension(certificate.get(), nullptr,
                                  NID_ext_key_usage, "serverAuth,clientAuth");
    if (!extensions_ok) return X509Ptr(nullptr, &X509_free);

    // Ed25519 signs with no separate digest (pass null).
    if (X509_sign(certificate.get(), issuer_key, nullptr) == 0) {
        return X509Ptr(nullptr, &X509_free);
    }
    return certificate;
}

std::string certificate_to_pem(X509* certificate) {
    BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) return {};
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return size > 0 ? std::string(data, static_cast<std::size_t>(size))
                    : std::string{};
}

std::string private_key_to_pem(EVP_PKEY* key) {
    BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
                                         nullptr, nullptr) != 1) {
        return {};
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return size > 0 ? std::string(data, static_cast<std::size_t>(size))
                    : std::string{};
}

X509Ptr certificate_from_pem(const std::string& pem) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
               &BIO_free);
    if (!bio) return X509Ptr(nullptr, &X509_free);
    return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                   &X509_free);
}

EvpKeyPtr private_key_from_pem(const std::string& pem) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
               &BIO_free);
    if (!bio) return EvpKeyPtr(nullptr, &EVP_PKEY_free);
    return EvpKeyPtr(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
        &EVP_PKEY_free);
}

// Does this stored certificate still belong to this cluster + node? True when
// it verifies against the derived CA key and its subject CN is the node id —
// the load-path check that regenerates identity after a token change.
bool certificate_matches(const std::string& certificate_pem,
                         EVP_PKEY* ca_key, const std::string& node_id) {
    X509Ptr certificate = certificate_from_pem(certificate_pem);
    if (!certificate) return false;
    if (X509_verify(certificate.get(), ca_key) != 1) return false;
    std::array<char, 256> common_name{};
    const int written = X509_NAME_get_text_by_NID(
        X509_get_subject_name(certificate.get()), NID_commonName,
        common_name.data(), static_cast<int>(common_name.size()));
    return written > 0 &&
           std::string_view(common_name.data(),
                            static_cast<std::size_t>(written)) == node_id;
}

// --- Secret-file persistence -------------------------------------------------

namespace fs = std::filesystem;

fs::path security_dir(const std::string& data_dir) {
    return fs::path(data_dir) / "security";
}

nlohmann::json_schema::json_validator& secret_validator() {
    static nlohmann::json_schema::json_validator validator(
        cluster_secret_schema());
    return validator;
}

// Collects the first validation failure without letting it cross the validator
// boundary as an exception (the node_telemetry.cpp pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// The secret file's contents: the join token, and the earlier ones.
struct SecretFile {
    std::string join_token;
    std::vector<std::string> retired_join_tokens;
};

// Parse + schema-validate the secret file's JSON.
std::optional<SecretFile> parse_secret_file(const std::string& contents,
                                            std::string* reason) {
    const nlohmann::json document =
        nlohmann::json::parse(contents, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_reason(reason, "cluster-secret.json: not valid JSON");
        return std::nullopt;
    }
    collecting_error_handler errors;
    secret_validator().validate(document, errors);
    if (errors.failed) {
        set_reason(reason, "cluster-secret.json: " + errors.reason);
        return std::nullopt;
    }
    SecretFile file;
    file.join_token = document["join_token"].get<std::string>();
    if (document.contains("retired_join_tokens")) {
        for (const nlohmann::json& token : document["retired_join_tokens"]) {
            file.retired_join_tokens.push_back(token.get<std::string>());
        }
    }
    return file;
}

}  // namespace

const nlohmann::json& cluster_secret_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ClusterSecret",
      "type": "object",
      "properties": {
        "version":    { "type": "integer", "const": 1 },
        "join_token": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
        "retired_join_tokens": {
          "type": "array",
          "maxItems": 4,
          "items": { "type": "string", "pattern": "^[0-9a-f]{64}$" }
        }
      },
      "required": ["version", "join_token"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

std::string generate_join_token() {
    std::array<unsigned char, kTokenBytes> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};  // CSPRNG failure — callers treat empty as an error
    }
    return to_hex(bytes.data(), bytes.size());
}

bool valid_join_token(std::string_view token) {
    if (token.size() != kTokenBytes * 2) return false;
    for (const char c : token) {
        if (!is_lower_hex(c)) return false;
    }
    return true;
}

std::optional<ClusterSecurity> derive_cluster_security(
    const std::string& join_token, const std::string& node_id,
    std::string* reason) {
    if (!valid_join_token(join_token)) {
        set_reason(reason,
                   "join token must be 64 lowercase hex characters");
        return std::nullopt;
    }
    if (node_id.empty()) {
        set_reason(reason, "node id must not be empty");
        return std::nullopt;
    }

    EvpKeyPtr ca_key = derive_ca_key(join_token);
    if (!ca_key) {
        set_reason(reason, "failed to derive the cluster CA key");
        return std::nullopt;
    }
    X509Ptr ca_certificate = make_certificate(kCaCommonName, ca_key.get(),
                                              kCaCommonName, ca_key.get(),
                                              /*is_ca=*/true);
    if (!ca_certificate) {
        set_reason(reason, "failed to build the cluster CA certificate");
        return std::nullopt;
    }

    EvpKeyPtr node_key = generate_node_key();
    if (!node_key) {
        set_reason(reason, "failed to generate the node keypair");
        return std::nullopt;
    }
    X509Ptr node_certificate = make_certificate(node_id, node_key.get(),
                                                kCaCommonName, ca_key.get(),
                                                /*is_ca=*/false);
    if (!node_certificate) {
        set_reason(reason, "failed to sign the node certificate");
        return std::nullopt;
    }

    ClusterSecurity security;
    security.join_token = join_token;
    security.ca_certificate_pem = certificate_to_pem(ca_certificate.get());
    security.certificate_pem = certificate_to_pem(node_certificate.get());
    security.private_key_pem = private_key_to_pem(node_key.get());
    if (!security.enabled()) {
        set_reason(reason, "failed to encode security material as PEM");
        return std::nullopt;
    }
    return security;
}

std::optional<ClusterSecurity> load_or_create_cluster_security(
    const std::string& data_dir, const std::string& node_id,
    const std::string& join_token_override, std::string* reason) {
    if (!join_token_override.empty() &&
        !valid_join_token(join_token_override)) {
        set_reason(reason,
                   "--join-token must be 64 lowercase hex characters");
        return std::nullopt;
    }

    const fs::path directory = security_dir(data_dir);
    const auto private_directory = util::PrivateDirectory::open(directory, reason);
    if (!private_directory) return std::nullopt;

    // Resolve the join token: an operator-passed override wins (joining an
    // existing cluster re-keys this node), else the stored secret, else a
    // fresh one (this node founds a cluster).
    std::string join_token = join_token_override;
    std::optional<std::string> stored;
    if (!private_directory->read("cluster-secret.json", stored, reason)) return std::nullopt;
    std::optional<SecretFile> stored_secret;
    if (stored) {
        stored_secret = parse_secret_file(*stored, reason);
        if (!stored_secret) return std::nullopt;  // malformed file: fail, don't clobber
    }
    if (join_token.empty() && stored_secret) join_token = stored_secret->join_token;
    // The earlier tokens, newest first: an override that replaces the stored
    // token retires it, so what was derived from it (the connector-credential
    // sealing key, #1230) can still be opened and re-sealed.
    std::vector<std::string> retired;
    if (stored_secret) {
        if (stored_secret->join_token != join_token) {
            retired.push_back(stored_secret->join_token);
        }
        for (const std::string& token : stored_secret->retired_join_tokens) {
            if (token != join_token &&
                std::find(retired.begin(), retired.end(), token) == retired.end()) {
                retired.push_back(token);
            }
        }
        if (retired.size() > kMaxRetiredJoinTokens) {
            retired.resize(kMaxRetiredJoinTokens);
        }
    }
    if (join_token.empty()) {
        join_token = generate_join_token();
        if (join_token.empty()) {
            set_reason(reason, "CSPRNG failure generating the join token");
            return std::nullopt;
        }
    }

    // Reuse the stored identity while it still belongs to this cluster + node;
    // otherwise mint a fresh one under the (possibly new) token's CA.
    EvpKeyPtr ca_key = derive_ca_key(join_token);
    if (!ca_key) {
        set_reason(reason, "failed to derive the cluster CA key");
        return std::nullopt;
    }
    std::optional<std::string> stored_certificate;
    std::optional<std::string> stored_key;
    if (!private_directory->read("node-cert.pem", stored_certificate, reason) ||
        !private_directory->read("node-key.pem", stored_key, reason)) return std::nullopt;
    std::optional<ClusterSecurity> security;
    if (stored_certificate && stored_key &&
        certificate_matches(*stored_certificate, ca_key.get(), node_id) &&
        private_key_from_pem(*stored_key)) {
        X509Ptr ca_certificate = make_certificate(kCaCommonName, ca_key.get(),
                                                  kCaCommonName, ca_key.get(),
                                                  /*is_ca=*/true);
        if (!ca_certificate) {
            set_reason(reason, "failed to build the cluster CA certificate");
            return std::nullopt;
        }
        security.emplace();
        security->join_token = join_token;
        security->ca_certificate_pem = certificate_to_pem(ca_certificate.get());
        security->certificate_pem = *stored_certificate;
        security->private_key_pem = *stored_key;
    } else {
        security = derive_cluster_security(join_token, node_id, reason);
        if (!security) return std::nullopt;
    }

    security->retired_join_tokens = retired;
    // Persist: the secret file (when new or overridden) and the identity.
    nlohmann::json secret_document = {{"version", 1},
                                      {"join_token", join_token}};
    if (!retired.empty()) secret_document["retired_join_tokens"] = retired;
    if (!stored || !join_token_override.empty()) {
        if (!private_directory->write("cluster-secret.json", secret_document.dump(2) + "\n", reason)) {
            return std::nullopt;
        }
    }
    if (!private_directory->write("node-cert.pem", security->certificate_pem, reason) ||
        !private_directory->write("node-key.pem", security->private_key_pem, reason)) {
        return std::nullopt;
    }
    return security;
}

}  // namespace zetta::cluster
