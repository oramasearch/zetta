#include "zetta/cluster/security.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "../util/private_directory.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
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

// --- Enrollment (#1319) -----------------------------------------------------

namespace {

constexpr const char* kEnrollmentCaCommonName = "zetta-cluster-enrollment-ca";

bool is_hex_of(std::string_view text, std::size_t bytes) {
    return text.size() == bytes * 2 && std::all_of(text.begin(), text.end(), is_lower_hex);
}

std::array<unsigned char, 32> decode_hex32(std::string_view hex) {
    return decode_token(hex);  // the same 32-byte lowercase-hex layout
}

X509Ptr certificate_from_der(std::span<const unsigned char> der) {
    if (der.empty() || der.size() > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        return X509Ptr(nullptr, &X509_free);
    }
    const unsigned char* cursor = der.data();
    X509Ptr certificate(d2i_X509(nullptr, &cursor, static_cast<long>(der.size())), &X509_free);
    if (!certificate || cursor != der.data() + der.size()) return X509Ptr(nullptr, &X509_free);
    return certificate;
}

std::string public_key_sha256(X509* certificate) {
    EVP_PKEY* key = X509_get0_pubkey(certificate);
    if (!key) return {};
    unsigned char* spki = nullptr;
    const int length = i2d_PUBKEY(key, &spki);
    if (length <= 0 || !spki) return {};
    const auto digest = sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(spki), static_cast<std::size_t>(length)));
    OPENSSL_free(spki);
    return to_hex(digest.data(), digest.size());
}

std::optional<std::string> only_common_name(X509* certificate) {
    X509_NAME* subject = X509_get_subject_name(certificate);
    const int position = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
    if (position < 0 || X509_NAME_get_index_by_NID(subject, NID_commonName, position) >= 0) {
        return std::nullopt;
    }
    const ASN1_STRING* value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, position));
    unsigned char* utf8 = nullptr;
    const int length = ASN1_STRING_to_UTF8(&utf8, value);
    if (length < 0 || !utf8) return std::nullopt;
    std::string name(reinterpret_cast<const char*>(utf8), static_cast<std::size_t>(length));
    OPENSSL_free(utf8);
    if (name.find('\0') != std::string::npos) return std::nullopt;
    return name;
}

std::string raw_public_key_hex(EVP_PKEY* key) {
    std::array<unsigned char, 32> raw{};
    std::size_t length = raw.size();
    if (!key || EVP_PKEY_get_raw_public_key(key, raw.data(), &length) != 1 ||
        length != raw.size()) {
        return {};
    }
    return to_hex(raw.data(), raw.size());
}

}  // namespace

std::optional<EnrollmentCa> enrollment_ca_from_seed(std::string_view seed_hex) {
    if (!is_hex_of(seed_hex, 32)) return std::nullopt;
    const auto seed = decode_hex32(seed_hex);
    EvpKeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                               seed.size()),
                  &EVP_PKEY_free);
    if (!key) return std::nullopt;
    X509Ptr certificate = make_certificate(kEnrollmentCaCommonName, key.get(),
                                           kEnrollmentCaCommonName, key.get(),
                                           /*is_ca=*/true);
    if (!certificate) return std::nullopt;
    EnrollmentCa ca;
    ca.seed = std::string(seed_hex);
    ca.certificate_pem = certificate_to_pem(certificate.get());
    ca.public_key = raw_public_key_hex(key.get());
    if (ca.certificate_pem.empty() || ca.public_key.empty()) return std::nullopt;
    return ca;
}

std::optional<EnrollmentCa> generate_enrollment_ca() {
    std::array<unsigned char, 32> seed{};
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) return std::nullopt;
    return enrollment_ca_from_seed(to_hex(seed.data(), seed.size()));
}

std::string certificate_public_key_sha256(std::span<const unsigned char> der) {
    X509Ptr certificate = certificate_from_der(der);
    return certificate ? public_key_sha256(certificate.get()) : std::string{};
}

std::string certificate_pem_public_key_sha256(std::string_view pem) {
    X509Ptr certificate = certificate_from_pem(std::string(pem));
    return certificate ? public_key_sha256(certificate.get()) : std::string{};
}

namespace {

std::string sign_enrollment_for_key(const EnrollmentCa& ca, const std::string& node_id,
                                    EVP_PKEY* subject_key);

EvpKeyPtr raw_public_key(std::string_view hex) {
    if (!is_hex_of(hex, 32)) return EvpKeyPtr(nullptr, &EVP_PKEY_free);
    const auto raw = decode_hex32(hex);
    return EvpKeyPtr(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(),
                                                 raw.size()),
                     &EVP_PKEY_free);
}

// The node id a node's own certificate names (its CN).
std::string node_id_of(const ClusterSecurity& security) {
    X509Ptr certificate = certificate_from_pem(security.certificate_pem);
    if (!certificate) return {};
    return only_common_name(certificate.get()).value_or(std::string{});
}

std::string spki_sha256(EVP_PKEY* key) {
    unsigned char* spki = nullptr;
    const int length = key ? i2d_PUBKEY(key, &spki) : 0;
    if (length <= 0 || !spki) return {};
    const auto digest = sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(spki), static_cast<std::size_t>(length)));
    OPENSSL_free(spki);
    return to_hex(digest.data(), digest.size());
}

}  // namespace

std::string sign_enrollment_certificate(const EnrollmentCa& ca, const std::string& node_id,
                                        std::span<const unsigned char> subject_der) {
    X509Ptr subject = certificate_from_der(subject_der);
    if (!subject) return {};
    return sign_enrollment_for_key(ca, node_id, X509_get0_pubkey(subject.get()));
}

std::string sign_enrollment_certificate_for_key(const EnrollmentCa& ca,
                                                const std::string& node_id,
                                                std::string_view raw_public_key_hex) {
    EvpKeyPtr key = raw_public_key(raw_public_key_hex);
    return key ? sign_enrollment_for_key(ca, node_id, key.get()) : std::string{};
}

std::string raw_public_key_sha256(std::string_view raw_public_key_hex) {
    EvpKeyPtr key = raw_public_key(raw_public_key_hex);
    return key ? spki_sha256(key.get()) : std::string{};
}

std::string node_raw_public_key(std::string_view private_key_pem) {
    EvpKeyPtr key = private_key_from_pem(std::string(private_key_pem));
    return key ? raw_public_key_hex(key.get()) : std::string{};
}

std::string sign_with_node_key(std::string_view private_key_pem, std::string_view message) {
    EvpKeyPtr key = private_key_from_pem(std::string(private_key_pem));
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                   &EVP_MD_CTX_free);
    if (!key || !context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return {};
    }
    std::array<unsigned char, 64> signature{};
    std::size_t length = signature.size();
    if (EVP_DigestSign(context.get(), signature.data(), &length,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) != 1 ||
        length != signature.size()) {
        return {};
    }
    return to_hex(signature.data(), length);
}

bool verify_with_raw_public_key(std::string_view raw_public_key_hex, std::string_view message,
                                std::string_view signature_hex) {
    EvpKeyPtr key = raw_public_key(raw_public_key_hex);
    if (!key || !is_hex_of(signature_hex, 64)) return false;
    std::vector<unsigned char> signature(64);
    for (std::size_t i = 0; i < 64; ++i) {
        const auto nibble = [](char c) {
            return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        signature[i] = static_cast<unsigned char>((nibble(signature_hex[i * 2]) << 4) |
                                                  nibble(signature_hex[i * 2 + 1]));
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                   &EVP_MD_CTX_free);
    return context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

bool certificate_signed_by(std::span<const unsigned char> der,
                           std::string_view ca_public_key_hex) {
    X509Ptr certificate = certificate_from_der(der);
    EvpKeyPtr ca_key = raw_public_key(ca_public_key_hex);
    return certificate && ca_key && X509_verify(certificate.get(), ca_key.get()) == 1 &&
           X509_cmp_current_time(X509_get0_notBefore(certificate.get())) <= 0 &&
           X509_cmp_current_time(X509_get0_notAfter(certificate.get())) >= 0;
}

std::string enrollment_ca_public_key_of(std::string_view ca_certificate_pem) {
    X509Ptr ca = certificate_from_pem(std::string(ca_certificate_pem));
    if (!ca) return {};
    EVP_PKEY* key = X509_get0_pubkey(ca.get());
    if (!key || X509_verify(ca.get(), key) != 1 ||
        only_common_name(ca.get()) != std::optional<std::string>(kEnrollmentCaCommonName)) {
        return {};
    }
    return raw_public_key_hex(key);
}

bool self_enroll(ClusterSecurity& security, std::string* reason) {
    const std::optional<EnrollmentCa> ca = enrollment_ca_from_seed(security.enrollment_ca_seed);
    if (!ca) {
        set_reason(reason, "this node holds no enrollment CA");
        return false;
    }
    const std::string certificate = sign_enrollment_certificate(
        *ca, node_id_of(security), certificate_pem_to_der(security.certificate_pem));
    if (certificate.empty() || node_id_of(security).empty()) {
        set_reason(reason, "failed to sign this node's enrollment certificate");
        return false;
    }
    if (!store_enrollment(security.directory, *ca, certificate, reason)) return false;
    security.enrolled_certificate_pem = certificate;
    security.cluster_ca_public_key = ca->public_key;
    return true;
}

bool provision_enrollment_ca(ClusterSecurity& security, std::string_view seed_hex,
                             std::string* reason) {
    const std::optional<EnrollmentCa> ca = enrollment_ca_from_seed(seed_hex);
    if (!ca) {
        set_reason(reason, "the enrollment CA seed must be 64 lowercase hex characters");
        return false;
    }
    if (!security.enrollment_ca_seed.empty() && security.enrollment_ca_seed != ca->seed) {
        set_reason(reason, "this node already holds another enrollment CA (cluster-ca.json)");
        return false;
    }
    if (!security.cluster_ca_public_key.empty() &&
        security.cluster_ca_public_key != ca->public_key) {
        set_reason(reason, "the enrollment CA seed does not match --cluster-ca");
        return false;
    }
    if (security.enrollment_ca_seed == ca->seed && !security.enrolled_certificate_pem.empty()) {
        return true;  // already provisioned
    }
    security.enrollment_ca_seed = ca->seed;
    return self_enroll(security, reason);
}

zetta::transport::TlsCredentials enrollment_credentials(const ClusterSecurity& security) {
    zetta::transport::TlsCredentials credentials;
    if (const auto ca = enrollment_ca_from_seed(security.enrollment_ca_seed)) {
        credentials.ca_certificate_pem = ca->certificate_pem;
    }
    credentials.certificate_pem = security.enrolled_certificate_pem;
    credentials.private_key_pem = security.private_key_pem;
    return credentials;
}

namespace {

std::string sign_enrollment_for_key(const EnrollmentCa& ca, const std::string& node_id,
                                    EVP_PKEY* subject_key) {
    if (node_id.empty() || !is_hex_of(ca.seed, 32) || !subject_key) return {};
    const auto seed = decode_hex32(ca.seed);
    EvpKeyPtr ca_key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(),
                                                  seed.size()),
                     &EVP_PKEY_free);
    X509Ptr certificate(X509_new(), &X509_free);
    if (!ca_key || !certificate || X509_set_version(certificate.get(), 2) != 1) return {};
    // A random 63-bit serial: every issuance is distinct, so a revocation names
    // one certificate (#1320).
    std::array<unsigned char, 8> serial_bytes{};
    if (RAND_bytes(serial_bytes.data(), static_cast<int>(serial_bytes.size())) != 1) return {};
    std::uint64_t serial = 0;
    for (const unsigned char byte : serial_bytes) serial = (serial << 8) | byte;
    serial &= ~(std::uint64_t{1} << 63);
    if (serial == 0) serial = 1;
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(certificate.get()), serial);
    // Valid from a minute ago (clock skew between members) for the window.
    if (!X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                         static_cast<long>(kEnrollmentValidityDays) * 24 * 60 * 60)) {
        return {};
    }
    if (!set_common_name(X509_get_subject_name(certificate.get()), node_id) ||
        !set_common_name(X509_get_issuer_name(certificate.get()), kEnrollmentCaCommonName) ||
        X509_set_pubkey(certificate.get(), subject_key) != 1) {
        return {};
    }
    if (!add_extension(certificate.get(), nullptr, NID_basic_constraints, "critical,CA:FALSE") ||
        !add_extension(certificate.get(), nullptr, NID_key_usage, "critical,digitalSignature") ||
        !add_extension(certificate.get(), nullptr, NID_ext_key_usage, "serverAuth,clientAuth")) {
        return {};
    }
    if (X509_sign(certificate.get(), ca_key.get(), nullptr) == 0) return {};
    return certificate_to_pem(certificate.get());
}

}  // namespace

std::optional<EnrolledCertificate> verify_enrolled_certificate(
    std::string_view certificate_pem, std::string_view ca_certificate_pem) {
    X509Ptr certificate = certificate_from_pem(std::string(certificate_pem));
    X509Ptr ca = certificate_from_pem(std::string(ca_certificate_pem));
    if (!certificate || !ca) return std::nullopt;
    EVP_PKEY* ca_key = X509_get0_pubkey(ca.get());
    // The CA certificate signs itself and names the enrollment CA; the leaf is
    // signed by it.
    if (!ca_key || X509_verify(ca.get(), ca_key) != 1 ||
        only_common_name(ca.get()) != std::optional<std::string>(kEnrollmentCaCommonName) ||
        X509_verify(certificate.get(), ca_key) != 1) {
        return std::nullopt;
    }
    if (X509_cmp_current_time(X509_get0_notBefore(certificate.get())) > 0 ||
        X509_cmp_current_time(X509_get0_notAfter(certificate.get())) < 0) {
        return std::nullopt;  // not yet valid, or expired
    }
    std::optional<std::string> node_id = only_common_name(certificate.get());
    if (!node_id || node_id->empty()) return std::nullopt;
    EnrolledCertificate enrolled;
    enrolled.node_id = std::move(*node_id);
    enrolled.public_key_sha256 = public_key_sha256(certificate.get());
    enrolled.ca_public_key = raw_public_key_hex(ca_key);
    std::uint64_t serial = 0;
    if (ASN1_INTEGER_get_uint64(&serial, X509_get0_serialNumber(certificate.get())) != 1) {
        return std::nullopt;
    }
    std::array<unsigned char, 8> serial_bytes{};
    for (int i = 7; i >= 0; --i, serial >>= 8) {
        serial_bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(serial & 0xff);
    }
    enrolled.serial = to_hex(serial_bytes.data(), serial_bytes.size());
    std::tm not_after{};
    if (ASN1_TIME_to_tm(X509_get0_notAfter(certificate.get()), &not_after) != 1) {
        return std::nullopt;
    }
    enrolled.not_after = static_cast<std::int64_t>(timegm(&not_after));
    if (enrolled.public_key_sha256.empty() || enrolled.ca_public_key.empty()) return std::nullopt;
    return enrolled;
}

std::string enrollment_proof(std::string_view join_token, std::string_view node_id,
                             std::string_view public_key_sha256) {
    std::string message = "zetta-enroll-v1\n";
    message += node_id;
    message += '\n';
    message += public_key_sha256;
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), join_token.data(), static_cast<int>(join_token.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(),
              mac.data(), &length)) {
        return {};
    }
    return to_hex(mac.data(), length);
}

bool enrollment_proof_matches(std::string_view join_token, std::string_view node_id,
                              std::string_view public_key_sha256, std::string_view proof) {
    const std::string expected = enrollment_proof(join_token, node_id, public_key_sha256);
    return !expected.empty() && expected.size() == proof.size() &&
           CRYPTO_memcmp(expected.data(), proof.data(), expected.size()) == 0;
}

std::vector<unsigned char> certificate_pem_to_der(std::string_view pem) {
    X509Ptr certificate = certificate_from_pem(std::string(pem));
    if (!certificate) return {};
    const int size = i2d_X509(certificate.get(), nullptr);
    if (size <= 0) return {};
    std::vector<unsigned char> der(static_cast<std::size_t>(size));
    unsigned char* cursor = der.data();
    if (i2d_X509(certificate.get(), &cursor) != size) return {};
    return der;
}

namespace {

nlohmann::json_schema::json_validator& validator_for(const nlohmann::json& schema) {
    // One validator per schema object; the schemas are function statics.
    static std::mutex mutex;
    static std::map<const nlohmann::json*, std::unique_ptr<nlohmann::json_schema::json_validator>>
        validators;
    const std::lock_guard lock(mutex);
    auto& slot = validators[&schema];
    if (!slot) slot = std::make_unique<nlohmann::json_schema::json_validator>(schema);
    return *slot;
}

bool validate_document(const nlohmann::json& document, const nlohmann::json& schema,
                       std::string_view file, std::string* reason) {
    collecting_error_handler errors;
    validator_for(schema).validate(document, errors);
    if (errors.failed) {
        set_reason(reason, std::string(file) + ": " + errors.reason);
        return false;
    }
    return true;
}

}  // namespace

bool load_node_pins(const std::string& directory, std::map<std::string, std::string>& pins,
                    std::string* reason) {
    pins.clear();
    if (directory.empty()) return true;
    const auto private_directory = util::PrivateDirectory::open(directory, reason);
    if (!private_directory) return false;
    std::optional<std::string> stored;
    if (!private_directory->read("node-pins.json", stored, reason)) return false;
    if (!stored) return true;
    const nlohmann::json document = nlohmann::json::parse(*stored, nullptr, false);
    if (document.is_discarded()) {
        set_reason(reason, "node-pins.json: not valid JSON");
        return false;
    }
    if (!validate_document(document, node_pins_schema(), "node-pins.json", reason)) return false;
    for (const auto& [node_id, hash] : document["pins"].items()) {
        pins.emplace(node_id, hash.get<std::string>());
    }
    return true;
}

bool store_node_pins(const std::string& directory,
                     const std::map<std::string, std::string>& pins, std::string* reason) {
    if (directory.empty()) return true;
    const auto private_directory = util::PrivateDirectory::open(directory, reason);
    if (!private_directory) return false;
    nlohmann::json document{{"version", 1}, {"pins", nlohmann::json::object()}};
    for (const auto& [node_id, hash] : pins) document["pins"][node_id] = hash;
    return private_directory->write("node-pins.json", document.dump(2) + "\n", reason);
}

bool store_enrollment(const std::string& directory, const EnrollmentCa& ca,
                      const std::string& enrolled_certificate_pem, std::string* reason) {
    if (directory.empty()) return true;
    const auto private_directory = util::PrivateDirectory::open(directory, reason);
    if (!private_directory) return false;
    const nlohmann::json document{{"version", 1}, {"seed", ca.seed}};
    return private_directory->write("cluster-ca.json", document.dump(2) + "\n", reason) &&
           (enrolled_certificate_pem.empty() ||
            private_directory->write("node-cert-enrolled.pem", enrolled_certificate_pem,
                                     reason));
}

const nlohmann::json& cluster_ca_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ClusterCa",
      "type": "object",
      "properties": {
        "version": { "type": "integer", "const": 1 },
        "seed":    { "type": "string", "pattern": "^[0-9a-f]{64}$" }
      },
      "required": ["version", "seed"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& node_pins_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.NodePins",
      "type": "object",
      "properties": {
        "version": { "type": "integer", "const": 1 },
        "pins": {
          "type": "object",
          "additionalProperties": { "type": "string", "pattern": "^[0-9a-f]{64}$" }
        }
      },
      "required": ["version", "pins"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& node_revocation_record_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.NodeRevocation",
      "type": "object",
      "properties": {
        "node_id":     { "type": "string", "minLength": 1, "maxLength": 128 },
        "serial":      { "type": "string", "maxLength": 16 },
        "revoked_by":  { "type": "string", "maxLength": 256 },
        "revoked_at":  { "type": "string" },
        "reason":      { "type": "string", "maxLength": 1024 },
        "allow_rekey": { "type": "boolean" }
      },
      "required": ["node_id", "serial", "revoked_by", "revoked_at", "reason", "allow_rekey"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& node_cert_record_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.NodeCert",
      "type": "object",
      "properties": {
        "certificate":       { "type": "string", "maxLength": 8192 },
        "ca_certificate":    { "type": "string", "maxLength": 8192 },
        "public_key_sha256": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
        "serial":            { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "issued_by":         { "type": "string", "maxLength": 128 },
        "not_after":         { "type": "integer" }
      },
      "required": ["certificate", "ca_certificate", "public_key_sha256", "serial",
                   "issued_by", "not_after"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

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

namespace {

// The token-derived material for `node_id` over an existing key: the node keeps
// its key across a token change, so the node id stays pinned to it (#1319).
std::optional<ClusterSecurity> derive_with_node_key(const std::string& join_token,
                                                    const std::string& node_id,
                                                    EVP_PKEY* node_key,
                                                    std::string* reason) {
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
    X509Ptr node_certificate = make_certificate(node_id, node_key,
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
    security.private_key_pem = private_key_to_pem(node_key);
    if (!security.enabled()) {
        set_reason(reason, "failed to encode security material as PEM");
        return std::nullopt;
    }
    return security;
}

}  // namespace

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

    EvpKeyPtr node_key = generate_node_key();
    if (!node_key) {
        set_reason(reason, "failed to generate the node keypair");
        return std::nullopt;
    }
    return derive_with_node_key(join_token, node_id, node_key.get(), reason);
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
    // A node that generates the token founds the cluster, and with it the
    // enrollment CA (#1319).
    const bool founding = join_token.empty();
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
    } else if (EvpKeyPtr kept = stored_key ? private_key_from_pem(*stored_key)
                                           : EvpKeyPtr(nullptr, &EVP_PKEY_free)) {
        // A new token (or node id): a new token-derived certificate over the key
        // this node already has, so the key its node id is enrolled with stays
        // the one it presents (#1319).
        security = derive_with_node_key(join_token, node_id, kept.get(), reason);
        if (!security) return std::nullopt;
    } else {
        security = derive_cluster_security(join_token, node_id, reason);
        if (!security) return std::nullopt;
    }

    security->retired_join_tokens = retired;

    // Enrollment (#1319): the enrollment CA, when this node holds it (the
    // founder generates it; every other node is handed it when it enrolls), and
    // this node's enrollment certificate while it still verifies, names this
    // node and carries the key it presents.
    security->enroll = true;
    security->directory = directory.string();
    std::optional<std::string> stored_ca;
    if (!private_directory->read("cluster-ca.json", stored_ca, reason)) return std::nullopt;
    std::optional<EnrollmentCa> ca;
    if (stored_ca) {
        const nlohmann::json document = nlohmann::json::parse(*stored_ca, nullptr, false);
        if (document.is_discarded() ||
            !validate_document(document, cluster_ca_schema(), "cluster-ca.json", reason)) {
            if (document.is_discarded()) set_reason(reason, "cluster-ca.json: not valid JSON");
            return std::nullopt;  // malformed: fail, never clobber
        }
        ca = enrollment_ca_from_seed(document["seed"].get<std::string>());
        if (!ca) {
            set_reason(reason, "cluster-ca.json: unusable seed");
            return std::nullopt;
        }
    } else if (founding) {
        ca = generate_enrollment_ca();
        if (!ca) {
            set_reason(reason, "failed to generate the enrollment CA");
            return std::nullopt;
        }
        if (!store_enrollment(security->directory, *ca, {}, reason)) return std::nullopt;
    }
    if (ca) {
        security->enrollment_ca_seed = ca->seed;
        std::optional<std::string> enrolled;
        if (!private_directory->read("node-cert-enrolled.pem", enrolled, reason)) {
            return std::nullopt;
        }
        if (enrolled) {
            const auto verified = verify_enrolled_certificate(*enrolled, ca->certificate_pem);
            if (verified && verified->node_id == node_id &&
                verified->public_key_sha256 ==
                    certificate_pem_public_key_sha256(security->certificate_pem)) {
                security->enrolled_certificate_pem = *enrolled;
            }
        }
        security->cluster_ca_public_key = ca->public_key;
        // A node holding the CA always has its own enrollment certificate before
        // the transport starts, since that is the certificate it presents (#1513).
        if (security->enrolled_certificate_pem.empty() && !self_enroll(*security, reason)) {
            return std::nullopt;
        }
    }
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
