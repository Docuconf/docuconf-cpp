// Shared test fixtures: temporary directories, certificates generated with
// OpenSSL, the gateway declaration (one input of every kind), and cue vet.
#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "docuconf/docuconf.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace testutil {

/// A fresh temporary directory, removed when the object goes away.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("docuconf-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
    /// Writes `content` at `rel` (creating directories) and returns the full path.
    std::string write(const std::string& rel, const std::string& content) const {
        fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
        return p.string();
    }
};

inline std::string read(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Replaces the value of metadata.generator.version in an exported contract. It
// is the project version, which every release PR bumps, so golden comparisons
// ignore it rather than needing a re-export per release.
inline std::string without_generator_version(const std::string& cue) {
    static const std::regex version(R"re((generator:\s*\{[^{}]*?\bversion:\s*)"[^"]*")re");
    return std::regex_replace(cue, version, "$1\"<generator-version>\"");
}

// ---- certificates ----

struct KeyCert {
    std::shared_ptr<EVP_PKEY> key;
    std::shared_ptr<X509> cert;
    std::string key_pem() const {
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(b, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
        char* d = nullptr;
        long n = BIO_get_mem_data(b, &d);
        std::string s(d, static_cast<std::size_t>(n));
        BIO_free(b);
        return s;
    }
    std::string cert_pem() const {
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(b, cert.get());
        char* d = nullptr;
        long n = BIO_get_mem_data(b, &d);
        std::string s(d, static_cast<std::size_t>(n));
        BIO_free(b);
        return s;
    }
};

struct CertOptions {
    std::string algorithm = "EC";  // EC, RSA, Ed25519
    std::string common_name = "test";
    std::vector<std::string> dns_names;
    long valid_from = -3600;               // seconds from now
    long valid_until = 90L * 24 * 3600;    // seconds from now
    bool is_ca = false;
    const KeyCert* issuer = nullptr;
};

inline std::shared_ptr<EVP_PKEY> make_key(const std::string& alg) {
    EVP_PKEY* k = nullptr;
    if (alg == "RSA") k = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(2048));
    else if (alg == "Ed25519") k = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    else k = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    return std::shared_ptr<EVP_PKEY>(k, EVP_PKEY_free);
}

inline KeyCert make_cert(const CertOptions& o) {
    KeyCert kc;
    kc.key = make_key(o.algorithm);
    X509* x = X509_new();
    X509_set_version(x, 2);
    static long serial = 1;
    ASN1_INTEGER_set(X509_get_serialNumber(x), serial++);
    X509_gmtime_adj(X509_getm_notBefore(x), o.valid_from);
    X509_gmtime_adj(X509_getm_notAfter(x), o.valid_until);
    X509_set_pubkey(x, kc.key.get());
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(o.common_name.c_str()),
                               -1, -1, 0);
    X509* issuer_cert = o.issuer ? o.issuer->cert.get() : x;
    EVP_PKEY* issuer_key = o.issuer ? o.issuer->key.get() : kc.key.get();
    X509_set_issuer_name(x, X509_get_subject_name(issuer_cert));
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer_cert, x, nullptr, nullptr, 0);
    auto add = [&](int nid, const std::string& value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    };
    if (o.is_ca) {
        add(NID_basic_constraints, "critical,CA:TRUE");
        add(NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add(NID_basic_constraints, "CA:FALSE");
    }
    if (!o.dns_names.empty()) {
        std::string san;
        for (const auto& n : o.dns_names) san += (san.empty() ? "DNS:" : ",DNS:") + n;
        add(NID_subject_alt_name, san);
    }
    const EVP_MD* md = EVP_PKEY_get_base_id(issuer_key) == EVP_PKEY_ED25519 ? nullptr : EVP_sha256();
    X509_sign(x, issuer_key, md);
    kc.cert = std::shared_ptr<X509>(x, X509_free);
    return kc;
}

/// A PKCS#12 keystore holding the key pair, protected by `password`.
inline std::string make_pkcs12(const KeyCert& kc, const std::string& password) {
    PKCS12* p12 = PKCS12_create(password.c_str(), "test", kc.key.get(), kc.cert.get(), nullptr, 0, 0, 0, 0, 0);
    BIO* b = BIO_new(BIO_s_mem());
    i2d_PKCS12_bio(b, p12);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string s(d, static_cast<std::size_t>(n));
    BIO_free(b);
    PKCS12_free(p12);
    return s;
}

/// An empty JKS keystore with a valid integrity digest for `password`.
inline std::string make_jks(const std::string& password) {
    std::string body = std::string("\xFE\xED\xFE\xED", 4) + std::string("\x00\x00\x00\x02", 4) +
                       std::string("\x00\x00\x00\x00", 4);
    std::string pre;
    for (char c : password) {
        pre += '\0';
        pre += c;
    }
    pre += "Mighty Aphrodite";
    unsigned char md[20];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, pre.data(), pre.size());
    EVP_DigestUpdate(ctx, body.data(), body.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    return body + std::string(reinterpret_cast<char*>(md), 20);
}

// ---- the gateway fixture: one input of every kind ----

enum class LogLevel { Debug, Info, Warn, Error };

struct RateLimits {
    std::uint32_t per_minute = 0;
    std::optional<std::uint32_t> burst;

    static json json_schema() {
        return json::parse(R"({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "title": "RateLimits",
            "description": "Bounds each client's request rate.",
            "type": "object",
            "properties": {
                "perMinute": {"type": "integer", "minimum": 1, "description": "Requests allowed per minute."},
                "burst": {"type": "integer", "minimum": 0, "description": "Extra requests allowed in a burst."}
            },
            "required": ["perMinute"],
            "additionalProperties": false
        })");
    }
};

inline void from_json(const json& j, RateLimits& r) {
    r.per_minute = j.at("perMinute").get<std::uint32_t>();
    if (j.contains("burst")) r.burst = j.at("burst").get<std::uint32_t>();
}
inline void to_json(json& j, const RateLimits& r) {
    j = json{{"perMinute", r.per_minute}};
    if (r.burst) j["burst"] = *r.burst;
}

struct Route {
    std::string match;
    std::string upstream;
    std::optional<std::string> timeout;
};

struct Routes {
    std::vector<Route> routes;

    static json json_schema() {
        return json::parse(R"({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "title": "Routes",
            "description": "The gateway's routing table file.",
            "type": "object",
            "properties": {
                "routes": {"type": "array", "minItems": 1, "items": {"$ref": "#/$defs/Route"},
                           "description": "Routes in match order."}
            },
            "required": ["routes"],
            "additionalProperties": false,
            "$defs": {
                "Route": {
                    "type": "object",
                    "description": "Sends requests under a path prefix to an upstream.",
                    "properties": {
                        "match": {"type": "string", "pattern": "^/", "description": "Path prefix the route matches."},
                        "upstream": {"type": "string", "pattern": "^https?://", "description": "Upstream base URL."},
                        "timeout": {"type": "string", "description": "Per-request timeout, such as 5s."}
                    },
                    "required": ["match", "upstream"],
                    "additionalProperties": false
                }
            }
        })");
    }
};

inline void from_json(const json& j, Route& r) {
    r.match = j.at("match").get<std::string>();
    r.upstream = j.at("upstream").get<std::string>();
    if (j.contains("timeout")) r.timeout = j.at("timeout").get<std::string>();
}
inline void from_json(const json& j, Routes& r) { r.routes = j.at("routes").get<std::vector<Route>>(); }

struct Gateway {
    CLI::App app{"gateway"};
    docuconf::Declaration config{app, "gateway"};
    std::vector<std::string> warnings;

    LogLevel log_level = LogLevel::Debug;
    std::string pod_namespace;
    std::optional<std::int64_t> memory_limit;
    RateLimits rate_limits;
    std::string partner_keystore_password;
    std::string database_url;
    std::uint16_t port = 0;
    std::chrono::milliseconds request_timeout{0};
    std::vector<std::string> allowed_origins;
    std::optional<std::vector<std::uint16_t>> extra_ports;
    std::optional<std::vector<std::int64_t>> shards;
    std::string stripe_api_base;
    double trace_sample_ratio = 0;
    bool debug = true;
    std::optional<std::string> region;
    std::optional<std::string> metrics_token;
    std::chrono::seconds cache_ttl{0};
    std::uint32_t cache_size = 0;

    docuconf::TlsKeyPair serving_tls;
    docuconf::ConfigFile<Routes> routes;
    docuconf::CaBundle upstream_ca;
    docuconf::Keystore partner_keystore;
    docuconf::TextFile license;
    docuconf::BinaryFile geoip;

    Gateway() {
        config.on_warning([this](const std::string& w) { warnings.push_back(w); });
        config.add_var("LOG_LEVEL", log_level, "Minimum log level emitted")
            .values({{"debug", LogLevel::Debug}, {"info", LogLevel::Info}, {"warn", LogLevel::Warn},
                      {"error", LogLevel::Error}})
            .default_val(LogLevel::Info)
            .group("logging");
        config.add_var("POD_NAMESPACE", pod_namespace, "Namespace the gateway runs in, for metrics labels");
        config.add_var("MEMORY_LIMIT", memory_limit, "Soft memory limit, in bytes").min(1);
        config.add_var("RATE_LIMITS", rate_limits, "Default per-client rate limits")
            .default_val(RateLimits{60, std::nullopt});
        config.add_var("PARTNER_KEYSTORE_PASSWORD", partner_keystore_password, "Password for the partner mTLS keystore")
            .secret();
        config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
            .secret()
            .schemes({"postgres", "postgresql"});
        config.add_var("PORT", port, "HTTP listen port").min(1).default_val(8080);
        config.add_var("REQUEST_TIMEOUT", request_timeout)
            .doc(R"(
                /// Upstream request timeout.
                ///
                /// The gateway gives up on an upstream after this long and answers 504.
                /// Raise it for slow batch endpoints; keep it below the load balancer's
                /// idle timeout, see @ref LoadBalancer::idle_timeout.
                ///
                /// # Choosing a value
                ///
                /// Measure the upstream's p99 latency first:
                /// @li p99 latency, from <tt>upstream_seconds</tt>
                /// @li retries, at most @c 3
                ///
                /// @code{.sh}
                /// histogram_quantile(0.99, upstream_seconds_bucket)
                /// @endcode
                ///
                /// @note Values are Go durations, such as @c 1m30s.
                /// @param ignored Function tags are dropped.
            )")
            .range("1s", "5m")
            .default_val("30s");
        config.add_var("ALLOWED_ORIGINS", allowed_origins, "CORS origins allowed to call the API")
            .min_items(1)
            .examples({"https://app.example.com"});
        config.add_var("EXTRA_PORTS", extra_ports, "Extra ports to listen on").max_items(4).item_min(1);
        config.add_var("SHARDS", shards, "Shard ids this instance owns").item_range(0, 1023).delimiter(";");
        config.add_var("STRIPE_API_BASE", stripe_api_base, "Stripe API base URL")
            .schemes({"https"})
            .default_val("https://api.stripe.com");
        config.add_var("TRACE_SAMPLE_RATIO", trace_sample_ratio, "Fraction of requests traced")
            .range(0, 1)
            .default_val(0.1);
        config.add_var("DEBUG", debug, "Serve the debug endpoints").default_val(false);
        config.add_var("REGION", region, "Cloud region, such as eu-west-1")
            .min_length(4)
            .max_length(32)
            .pattern("^[a-z]{2}-[a-z]+-[0-9]$")
            .deprecated("Read from the node's topology labels instead");
        config.add_var("METRICS_TOKEN", metrics_token, "API token for the metrics backend").secret().min_length(20);
        config.add_var("CACHE__TTL", cache_ttl, "Cache entry lifetime").default_val("5m");
        config.add_var("CACHE__SIZE", cache_size, "Maximum cached entries").default_val(1000);

        config.add_file("serving-tls", serving_tls, "Certificate the gateway serves HTTPS with")
            .path("/etc/gateway/tls")
            .required()
            .dns_names({"gateway.internal", "api.example.com"})
            .key_algorithms({"ECDSA", "RSA"})
            .min_remaining("720h");
        config.add_file("routes", routes, "Routing table: path prefixes and their upstreams")
            .details("Each route maps a path prefix to an upstream URL.\n\nThe longest prefix wins.")
            .path("/etc/gateway/routes/routes.yaml")
            .path_env("ROUTES_FILE")
            .required()
            .max_size(65536);
        config.add_file("upstream-ca", upstream_ca, "Private CA for upstream services")
            .path("/etc/gateway/upstream-ca/ca.pem")
            .min_certificates(1);
        config.add_file("partner-keystore", partner_keystore, "Client certificate for mTLS to the partner API")
            .path("/etc/gateway/partner/keystore.p12")
            .password_var("PARTNER_KEYSTORE_PASSWORD");
        config.add_file("license", license, "Gateway licence key")
            .path("/etc/gateway/license/license.key")
            .required()
            .pattern("^[A-Z0-9]{5}(-[A-Z0-9]{5}){3}\\n?$");
        config.add_file("geoip", geoip, "GeoIP database for country-based routing")
            .path("/data/geoip/GeoLite2-City.mmdb")
            .max_size(134217728);
    }
};

// ---- cue vet ----

inline std::string find_cue() {
    if (const char* c = std::getenv("CUE"); c && *c) return c;
    if (const char* h = std::getenv("HOME")) {
        std::string p = std::string(h) + "/go/bin/cue";
        if (fs::exists(p)) return p;
    }
    if (std::system("command -v cue >/dev/null 2>&1") == 0) return "cue";
    return "";
}

inline std::string spec_cue_dir() {
    if (const char* p = std::getenv("DOCUCONF_SPEC_CUE"); p && *p) return p;
    return std::string(DOCUCONF_SOURCE_DIR) + "/../docuconf-go/spec/cue";
}

/// Runs `cue vet -c` on a contract against the meta-schema. Returns the
/// output on failure, nullopt on success; throws std::runtime_error when
/// cue or the meta-schema is missing (callers skip).
inline std::optional<std::string> cue_vet(const std::string& contract_cue) {
    std::string cue = find_cue();
    std::string spec = spec_cue_dir();
    if (cue.empty()) throw std::runtime_error("cue not found (set CUE)");
    if (!fs::exists(fs::path(spec) / "contract")) throw std::runtime_error("meta-schema not found at " + spec);
    TempDir dir;
    fs::copy(fs::path(spec) / "cue.mod", dir.path / "cue.mod", fs::copy_options::recursive);
    fs::copy(fs::path(spec) / "contract", dir.path / "contract", fs::copy_options::recursive);
    dir.write("svc/contract.cue", contract_cue);
    std::string cmd = "cd '" + dir.str() + "' && '" + cue + "' vet -c ./svc 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    std::string out;
    std::array<char, 4096> buf{};
    while (std::size_t n = fread(buf.data(), 1, buf.size(), p)) out.append(buf.data(), n);
    int rc = pclose(p);
    if (rc == 0) return std::nullopt;
    return out;
}

}  // namespace testutil
