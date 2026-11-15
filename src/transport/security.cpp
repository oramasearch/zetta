// SPDX-License-Identifier: MIT

#include "zetta/transport/security.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace zetta::transport {
namespace {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using EvpKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using X509StorePtr = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
using X509StoreContextPtr = std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>;
struct X509StackDeleter {
    void operator()(STACK_OF(X509)* stack) const {
        sk_X509_free(stack);
    }
};
using X509StackPtr = std::unique_ptr<STACK_OF(X509), X509StackDeleter>;

void set_reason(std::string* reason, std::string message) {
    if (reason) {
        *reason = std::move(message);
    }
}

std::optional<std::string> read_file(const std::filesystem::path& path, std::string* reason) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_reason(reason, path.string() + ": cannot open");
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        set_reason(reason, path.string() + ": cannot read");
        return std::nullopt;
    }
    return contents.str();
}

std::vector<X509Ptr> parse_certificates(std::string_view pem, std::string_view field,
                                        std::string* reason) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    if (!bio) {
        set_reason(reason, std::string(field) + ": cannot allocate PEM reader");
        return {};
    }

    std::vector<X509Ptr> certificates;
    while (X509* certificate = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)) {
        certificates.emplace_back(certificate, &X509_free);
    }
    ERR_clear_error();
    if (certificates.empty()) {
        set_reason(reason, std::string(field) + ": no valid PEM certificate");
    }
    return certificates;
}

std::string certificate_to_pem(X509* certificate) {
    BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) {
        return {};
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
}

EvpKeyPtr parse_private_key(std::string_view pem, std::string* reason) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    if (!bio) {
        set_reason(reason, "private_key_pem: cannot allocate PEM reader");
        return EvpKeyPtr(nullptr, &EVP_PKEY_free);
    }
    EvpKeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free);
    if (!key) {
        ERR_clear_error();
        set_reason(reason, "private_key_pem: no valid PEM private key");
    }
    return key;
}

bool verify_credentials(const TlsCredentials& credentials, std::string* reason) {
    if (!credentials.complete()) {
        set_reason(reason, "mutual TLS requires a CA certificate, certificate, and private key");
        return false;
    }

    std::vector<X509Ptr> chain =
        parse_certificates(credentials.certificate_pem, "certificate_pem", reason);
    if (chain.empty()) {
        return false;
    }
    EvpKeyPtr private_key = parse_private_key(credentials.private_key_pem, reason);
    if (!private_key) {
        return false;
    }
    if (X509_check_private_key(chain.front().get(), private_key.get()) != 1) {
        ERR_clear_error();
        set_reason(reason, "private_key_pem: key does not match the leaf certificate");
        return false;
    }

    std::vector<X509Ptr> authorities =
        parse_certificates(credentials.ca_certificate_pem, "ca_certificate_pem", reason);
    if (authorities.empty()) {
        return false;
    }
    X509StorePtr store(X509_STORE_new(), &X509_STORE_free);
    if (!store) {
        set_reason(reason, "ca_certificate_pem: cannot allocate trust store");
        return false;
    }
    for (const X509Ptr& authority : authorities) {
        if (X509_STORE_add_cert(store.get(), authority.get()) != 1) {
            ERR_clear_error();
            set_reason(reason, "ca_certificate_pem: cannot add certificate to trust store");
            return false;
        }
    }

    X509StackPtr intermediates(sk_X509_new_null());
    if (!intermediates) {
        set_reason(reason, "certificate_pem: cannot allocate certificate chain");
        return false;
    }
    for (std::size_t index = 1; index < chain.size(); ++index) {
        if (sk_X509_push(intermediates.get(), chain[index].get()) == 0) {
            set_reason(reason, "certificate_pem: cannot build certificate chain");
            return false;
        }
    }

    X509StoreContextPtr context(X509_STORE_CTX_new(), &X509_STORE_CTX_free);
    if (!context || X509_STORE_CTX_init(context.get(), store.get(), chain.front().get(),
                                        intermediates.get()) != 1) {
        set_reason(reason, "certificate_pem: cannot initialize verification");
        return false;
    }
    if (X509_verify_cert(context.get()) != 1) {
        const int error = X509_STORE_CTX_get_error(context.get());
        set_reason(reason, std::string("certificate_pem: ") + X509_verify_cert_error_string(error));
        return false;
    }
    return true;
}

bool apply_context_step(const std::error_code& error, std::string_view field, std::string* reason) {
    if (!error) {
        return true;
    }
    set_reason(reason, std::string(field) + ": " + error.message());
    return false;
}

}  // namespace

std::optional<TlsCredentials> load_tls_credentials(const TlsCredentialFiles& files,
                                                   std::string* reason) {
    std::optional<std::string> ca = read_file(files.ca_certificate, reason);
    if (!ca) {
        return std::nullopt;
    }
    std::optional<std::string> certificate = read_file(files.certificate, reason);
    if (!certificate) {
        return std::nullopt;
    }
    std::optional<std::string> private_key = read_file(files.private_key, reason);
    if (!private_key) {
        return std::nullopt;
    }
    return TlsCredentials{.ca_certificate_pem = std::move(*ca),
                          .certificate_pem = std::move(*certificate),
                          .private_key_pem = std::move(*private_key)};
}

std::optional<boost::corosio::tls_context> make_mutual_tls_context(
    const TlsCredentials& credentials, std::string* reason) {
    if (!verify_credentials(credentials, reason)) {
        return std::nullopt;
    }

    boost::corosio::tls_context context;
    if (!apply_context_step(context.use_certificate_chain(credentials.certificate_pem),
                            "certificate_pem", reason) ||
        !apply_context_step(context.use_private_key(credentials.private_key_pem,
                                                    boost::corosio::tls_file_format::pem),
                            "private_key_pem", reason)) {
        return std::nullopt;
    }

    std::vector<X509Ptr> authorities =
        parse_certificates(credentials.ca_certificate_pem, "ca_certificate_pem", reason);
    for (const X509Ptr& authority : authorities) {
        const std::string authority_pem = certificate_to_pem(authority.get());
        if (authority_pem.empty() ||
            !apply_context_step(context.add_certificate_authority(authority_pem),
                                "ca_certificate_pem", reason)) {
            return std::nullopt;
        }
    }

    if (!apply_context_step(context.set_verify_mode(boost::corosio::tls_verify_mode::require_peer),
                            "verify_mode", reason) ||
        !apply_context_step(context.set_min_protocol_version(boost::corosio::tls_version::tls_1_3),
                            "minimum_protocol_version", reason) ||
        !apply_context_step(context.set_max_protocol_version(boost::corosio::tls_version::tls_1_3),
                            "maximum_protocol_version", reason)) {
        return std::nullopt;
    }
    return context;
}

std::optional<std::string> certificate_common_name(std::span<const unsigned char> der) {
    if (der.empty() || der.size() > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        return std::nullopt;
    }
    const unsigned char* cursor = der.data();
    X509Ptr certificate(d2i_X509(nullptr, &cursor, static_cast<long>(der.size())), &X509_free);
    // One certificate and nothing after it.
    if (!certificate || cursor != der.data() + der.size()) {
        return std::nullopt;
    }
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    const int position = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
    if (position < 0 || X509_NAME_get_index_by_NID(subject, NID_commonName, position) >= 0) {
        return std::nullopt;  // no common name, or an ambiguous second one
    }
    const ASN1_STRING* value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, position));
    unsigned char* utf8 = nullptr;
    const int length = ASN1_STRING_to_UTF8(&utf8, value);
    if (length < 0 || utf8 == nullptr) {
        return std::nullopt;
    }
    std::string common_name(reinterpret_cast<const char*>(utf8), static_cast<std::size_t>(length));
    OPENSSL_free(utf8);
    // An embedded NUL would let "a\0b" pass as "a" in a C-string compare.
    if (common_name.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return common_name;
}

}  // namespace zetta::transport
