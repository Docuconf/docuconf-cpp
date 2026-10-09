// TLS key pairs, CA bundles and keystores, checked with OpenSSL.
#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "docuconf/duration.hpp"
#include "internal.hpp"

namespace docuconf {
namespace detail {
namespace {

struct Free {
    void operator()(BIO* p) const { BIO_free(p); }
    void operator()(X509* p) const { X509_free(p); }
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
    void operator()(X509_STORE* p) const { X509_STORE_free(p); }
    void operator()(X509_STORE_CTX* p) const { X509_STORE_CTX_free(p); }
    void operator()(PKCS12* p) const { PKCS12_free(p); }
    void operator()(STACK_OF(X509) * p) const { sk_X509_pop_free(p, X509_free); }
};
template <class T>
using Ptr = std::unique_ptr<T, Free>;

Ptr<BIO> mem(const std::string& s) { return Ptr<BIO>(BIO_new_mem_buf(s.data(), static_cast<int>(s.size()))); }

std::size_t count_blocks(const std::string& pem) {
    std::size_t n = 0, pos = 0;
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    while ((pos = pem.find(begin, pos)) != std::string::npos) {
        ++n;
        pos += begin.size();
    }
    return n;
}

// Every PEM certificate in `pem`; false when a certificate block does not
// parse.
bool parse_certs(const std::string& pem, std::vector<Ptr<X509>>& out) {
    auto bio = mem(pem);
    while (X509* x = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)) out.emplace_back(x);
    ERR_clear_error();
    return out.size() == count_blocks(pem);
}

std::string iso_time(const ASN1_TIME* t) {
    struct tm tm {};
    if (ASN1_TIME_to_tm(t, &tm) != 1) return "?";
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string key_algorithm(EVP_PKEY* k) {
    switch (EVP_PKEY_get_base_id(k)) {
        case EVP_PKEY_RSA: return "RSA";
        case EVP_PKEY_EC: return "ECDSA";
        case EVP_PKEY_ED25519: return "Ed25519";
        default: return OBJ_nid2sn(EVP_PKEY_get_base_id(k));
    }
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + v[i];
    return out;
}

}  // namespace

std::vector<Problem> check_tls(const FileSpec& spec, const std::string& crt, const std::string& key,
                               const std::string* ca) {
    std::vector<Problem> out;
    std::vector<Ptr<X509>> certs;
    // SPEC §11.2 item 5: no PEM certificate at all is file_malformed; a PEM
    // certificate that does not parse is certificate_invalid.
    bool parsed = parse_certs(crt, certs);
    if (!parsed || certs.empty()) {
        if (parsed) out.emplace_back(Code::FileMalformed, "tls.crt holds no PEM certificate");
        else out.emplace_back(Code::CertificateInvalid, "tls.crt holds a certificate that does not parse");
        return out;
    }
    X509* leaf = certs[0].get();
    auto kb = mem(key);
    Ptr<EVP_PKEY> pkey(PEM_read_bio_PrivateKey(kb.get(), nullptr, nullptr, const_cast<char*>("")));
    ERR_clear_error();
    if (!pkey) {
        out.emplace_back(Code::FileMalformed, "tls.key holds no unencrypted PEM private key");
    } else if (X509_check_private_key(leaf, pkey.get()) != 1) {
        ERR_clear_error();
        out.emplace_back(Code::KeyMismatch, "tls.key does not match the certificate in tls.crt");
    }

    const ASN1_TIME* not_before = X509_get0_notBefore(leaf);
    const ASN1_TIME* not_after = X509_get0_notAfter(leaf);
    if (X509_cmp_current_time(not_before) > 0) {
        out.emplace_back(Code::CertificateInvalid, "certificate is not valid until " + iso_time(not_before));
    } else if (X509_cmp_current_time(not_after) < 0) {
        out.emplace_back(Code::CertificateInvalid, "certificate expired at " + iso_time(not_after));
    } else if (spec.min_remaining) {
        int days = 0, secs = 0;
        if (ASN1_TIME_diff(&days, &secs, nullptr, not_after) == 1) {
            std::int64_t left = static_cast<std::int64_t>(days) * 86400 + secs;
            std::int64_t need = std::chrono::duration_cast<std::chrono::seconds>(*spec.min_remaining).count();
            if (left < need) {
                out.emplace_back(Code::CertificateExpiring,
                                 "certificate expires at " + iso_time(not_after) + ", in " +
                                     format_go_duration(std::chrono::seconds(left - left % 3600)) +
                                     ", less than minRemaining " + format_go_duration(*spec.min_remaining));
            }
        }
    }

    for (const auto& name : spec.dns_names) {
        unsigned flags = X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT;
        if (X509_check_host(leaf, name.data(), name.size(), flags, nullptr) != 1) {
            out.emplace_back(Code::CertificateNameMismatch, "certificate does not cover " + name);
        }
    }

    if (!spec.key_algorithms.empty()) {
        EVP_PKEY* pub = X509_get0_pubkey(leaf);
        std::string alg = pub ? key_algorithm(pub) : "unknown";
        if (std::find(spec.key_algorithms.begin(), spec.key_algorithms.end(), alg) == spec.key_algorithms.end())
            out.emplace_back(Code::CertificateInvalid,
                             "certificate key is " + alg + ", not one of " + join(spec.key_algorithms));
    }

    if (ca) {
        std::vector<Ptr<X509>> roots;
        bool ca_parsed = parse_certs(*ca, roots);
        if (!ca_parsed || roots.empty()) {
            if (ca_parsed) out.emplace_back(Code::FileMalformed, "ca.crt holds no PEM certificate");
            else out.emplace_back(Code::CertificateInvalid, "ca.crt holds a certificate that does not parse");
            return out;
        }
        Ptr<X509_STORE> store(X509_STORE_new());
        for (auto& r : roots) X509_STORE_add_cert(store.get(), r.get());
        // Expiry is reported above; an intermediate CA may be the anchor.
        X509_STORE_set_flags(store.get(), X509_V_FLAG_PARTIAL_CHAIN | X509_V_FLAG_NO_CHECK_TIME);
        Ptr<STACK_OF(X509)> chain(sk_X509_new_null());
        for (std::size_t i = 1; i < certs.size(); ++i) {
            X509_up_ref(certs[i].get());
            sk_X509_push(chain.get(), certs[i].get());
        }
        Ptr<X509_STORE_CTX> ctx(X509_STORE_CTX_new());
        X509_STORE_CTX_init(ctx.get(), store.get(), leaf, chain.get());
        if (X509_verify_cert(ctx.get()) != 1) {
            int err = X509_STORE_CTX_get_error(ctx.get());
            out.emplace_back(Code::CertificateInvalid,
                             std::string("certificate does not chain to ca.crt: ") + X509_verify_cert_error_string(err));
        }
        ERR_clear_error();
    }
    return out;
}

std::vector<Problem> check_ca_bundle(const FileSpec& spec, const std::string& pem, std::size_t& count) {
    std::vector<Ptr<X509>> certs;
    std::vector<Problem> out;
    if (!parse_certs(pem, certs)) {
        out.emplace_back(Code::CertificateInvalid, "holds a PEM certificate that does not parse");
        return out;
    }
    count = certs.size();
    if (count < spec.min_certificates) {
        out.emplace_back(Code::FileMalformed, "holds " + std::to_string(count) + " PEM certificate" +
                                                  (count == 1 ? "" : "s") + ", below minCertificates " +
                                                  std::to_string(spec.min_certificates));
    }
    return out;
}

namespace {

// UTF-8 to UTF-16BE, as Java encodes a keystore password.
std::string utf16be(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp;
        std::size_t n;
        if (c < 0x80) cp = c, n = 0;
        else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, n = 1;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, n = 2;
        else cp = c & 0x07, n = 3;
        for (std::size_t k = 1; k <= n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += n + 1;
        auto unit = [&](std::uint32_t u) {
            out += static_cast<char>((u >> 8) & 0xFF);
            out += static_cast<char>(u & 0xFF);
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return out;
}

// JKS has no OpenSSL parser. Its integrity check is
// SHA-1(password as UTF-16BE || "Mighty Aphrodite" || body) == trailer.
std::vector<Problem> check_jks(const std::string& data, const std::string& password) {
    std::vector<Problem> out;
    auto u = [&](std::size_t i) { return static_cast<unsigned char>(data[i]); };
    if (data.size() < 12 + 20 || !(u(0) == 0xFE && u(1) == 0xED && u(2) == 0xFE && u(3) == 0xED)) {
        out.emplace_back(Code::KeystoreUnreadable, "is not a JKS keystore");
        return out;
    }
    std::string pre = utf16be(password) + "Mighty Aphrodite";
    std::size_t body = data.size() - 20;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx.get(), pre.data(), pre.size());
    EVP_DigestUpdate(ctx.get(), data.data(), body);
    EVP_DigestFinal_ex(ctx.get(), md, &len);
    if (len != 20 || std::memcmp(md, data.data() + body, 20) != 0)
        out.emplace_back(Code::KeystoreUnreadable,
                         "does not open with its password (wrong password, or the keystore is corrupt)");
    return out;
}

}  // namespace

std::vector<Problem> check_keystore(const FileSpec& spec, const std::string& data, const std::string& password) {
    if (spec.format == "jks") return check_jks(data, password);
    std::vector<Problem> out;
    // Keystores written by older tools use algorithms OpenSSL 3 keeps in
    // its legacy provider.
    static bool legacy = [] {
        OSSL_PROVIDER_try_load(nullptr, "legacy", 1);
        ERR_clear_error();
        return true;
    }();
    (void)legacy;
    auto bio = mem(data);
    Ptr<PKCS12> p12(d2i_PKCS12_bio(bio.get(), nullptr));
    if (!p12) {
        ERR_clear_error();
        out.emplace_back(Code::KeystoreUnreadable, "is not a PKCS#12 keystore");
        return out;
    }
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    int ok = PKCS12_parse(p12.get(), password.c_str(), &pkey, &cert, &ca);
    if (!ok && password.empty()) {
        ERR_clear_error();
        ok = PKCS12_parse(p12.get(), nullptr, &pkey, &cert, &ca);
    }
    ERR_clear_error();
    EVP_PKEY_free(pkey);
    X509_free(cert);
    sk_X509_pop_free(ca, X509_free);
    if (!ok)
        out.emplace_back(Code::KeystoreUnreadable,
                         "does not open with " +
                             (spec.password_var.empty() ? std::string("an empty password") : spec.password_var) +
                             " (wrong password, or the keystore is corrupt)");
    return out;
}

}  // namespace detail
}  // namespace docuconf
