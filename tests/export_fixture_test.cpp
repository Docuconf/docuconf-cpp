// The shared export fixture (SPEC §11.2 item 3, §12): docuconf-go's
// conformance/export/fixture.yaml, declared with this SDK's API, exported,
// and compared with conformance/export/golden.cue by
// `docuconf conformance export`.
//
// The golden contract is found next to the conformance cases
// (DOCUCONF_CONFORMANCE's directory, then ../docuconf-go/conformance), and
// the CLI through DOCUCONF_CLI, then `docuconf` on PATH. Without either the
// test skips, unless DOCUCONF_REQUIRE_CONFORMANCE=1 (CI sets it).
//
// One known difference: the fixture declares `reload: watch` on `settings`
// and `serving-tls`. docuconf reads file inputs once, at boot, and rejects
// `watch` at declaration time (SPEC §11.2 item 8), so those two inputs
// export the default, `restart`. The test requires exactly those two
// differences and no other.
#include <gtest/gtest.h>

#include <sstream>
#include <sys/wait.h>

#include "test_util.hpp"

namespace fixture {

struct RateLimits {
    std::int64_t perMinute = 60;
    std::optional<std::int64_t> burst;
    static json json_schema() {
        return json::parse(R"({"type": "object", "required": ["perMinute"], "additionalProperties": false,
            "properties": {"perMinute": {"type": "integer", "minimum": 1},
                           "burst": {"type": "integer", "minimum": 0}}})");
    }
};
inline void to_json(json& j, const RateLimits& r) {
    j = json{{"perMinute", r.perMinute}};
    if (r.burst) j["burst"] = *r.burst;
}
inline void from_json(const json& j, RateLimits& r) {
    j.at("perMinute").get_to(r.perMinute);
    if (j.contains("burst")) r.burst = j.at("burst").get<std::int64_t>();
}

struct Settings {
    std::string name;
    std::int64_t replicas = 1;
    std::optional<std::vector<std::string>> tags;
    static json json_schema() {
        return json::parse(R"({"type": "object", "required": ["name", "replicas"], "additionalProperties": false,
            "properties": {"name": {"type": "string", "minLength": 1},
                           "replicas": {"type": "integer", "minimum": 1},
                           "tags": {"type": "array", "items": {"type": "string"}}}})");
    }
};
inline void from_json(const json& j, Settings& s) {
    j.at("name").get_to(s.name);
    j.at("replicas").get_to(s.replicas);
    if (j.contains("tags")) s.tags = j.at("tags").get<std::vector<std::string>>();
}

/// conformance/export/fixture.yaml, in docuconf-cpp's declaration API.
struct Fixture {
    CLI::App app{"docuconf-fixture"};
    docuconf::Declaration config{app, "docuconf-fixture"};

    std::string app_name;
    std::string database_url;
    int port = 0;
    double trace_ratio = 0;
    bool debug = false;
    std::chrono::nanoseconds request_timeout{};
    std::string log_level;
    std::optional<std::vector<std::string>> allowed_origins;
    std::optional<std::vector<std::int64_t>> shards;
    std::optional<docuconf::KeySet> webhook_keys;
    RateLimits rate_limits;
    std::optional<std::int64_t> old_port;
    std::optional<std::string> partner_password;

    docuconf::ConfigFile<Settings> settings, rules, flags;
    docuconf::TlsKeyPair serving_tls;
    docuconf::CaBundle trust;
    docuconf::Keystore partner;
    docuconf::TextFile licence;
    docuconf::BinaryFile geoip, geo_db;

    Fixture() {
        using namespace std::chrono_literals;
        config.app_version("1.0.0");
        config.add_var("APP_NAME", app_name)
            .doc("Service name, used in logs and metrics\n\nLower case, as a DNS label allows.")
            .default_val("orders")
            .min_length(2)
            .max_length(40)
            .pattern("^[a-z][a-z0-9-]*$")
            .group("general")
            .examples({"orders", "billing"})
            .config_key("App:Name");
        config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
            .required()
            .secret()
            .schemes({"postgres", "postgresql"})
            .max_length(2048)
            .group("database");
        config.add_var("PORT", port, "HTTP listen port").default_val(8080).range(1, 65535);
        config.add_var("TRACE_RATIO", trace_ratio, "Fraction of requests traced").default_val(0.25).range(0, 1);
        config.add_var("DEBUG", debug, "Serve the debug endpoints").default_val(false);
        config.add_var("REQUEST_TIMEOUT", request_timeout, "Upstream request timeout")
            .default_val(90s)
            .range(1s, 5min);
        config.add_var("LOG_LEVEL", log_level, "Minimum log level")
            .values({"debug", "info", "warn", "error"})
            .default_val("info");
        config.add_var("ALLOWED_ORIGINS", allowed_origins, "CORS origins allowed to call the API")
            .min_items(1)
            .max_items(5)
            .item_min_length(1)
            .item_max_length(255)
            .delimiter(";");
        config.add_var("SHARDS", shards, "Shards this instance owns").item_range(0, 1023);
        config.add_var("WEBHOOK_KEYS", webhook_keys, "Keys that verify webhook signatures").key_length(32, 256);
        config.add_var("RATE_LIMITS", rate_limits, "Per-client rate limits")
            .default_val(RateLimits{60, std::nullopt})
            .max_length(1024);
        config.add_var("OLD_PORT", old_port, "Port the service used to listen on").deprecated("Use PORT instead", "PORT");
        config.add_var("PARTNER_PASSWORD", partner_password, "Password of the partner keystore").secret();

        config.add_file("settings", settings, "Application settings")
            .path("/etc/app/settings/settings.json")
            .format("json")
            .required()
            .path_env("SETTINGS_FILE")
            .max_size(65536)
            .group("general");  // and reload watch, which this SDK rejects
        config.add_file("rules", rules, "Routing rules").path("/etc/app/rules/rules.yaml").format("yaml");
        config.add_file("flags", flags, "Feature defaults").path("/etc/app/flags/flags.toml").format("toml");
        config.add_file("serving-tls", serving_tls, "Certificate the service serves HTTPS with")
            .path("/etc/app/tls")
            .dns_names({"app.example.test", "api.example.test"})
            .key_algorithms({"ECDSA", "Ed25519"})
            .min_remaining(720h)
            .require_ca();  // and reload watch, which this SDK rejects
        config.add_file("trust", trust, "CAs the service trusts").path("/etc/app/trust/bundle.pem").min_certificates(2);
        config.add_file("partner", partner, "Client certificate for the partner API")
            .path("/etc/app/partner/keystore.p12")
            .format("pkcs12")
            .password_var("PARTNER_PASSWORD");
        config.add_file("licence", licence, "Licence key")
            .path("/etc/app/licence/licence.key")
            .min_length(8)
            .max_length(64)
            .pattern("^[A-Z0-9-]+\\n?$");
        config.add_file("geoip", geoip, "GeoIP database")
            .path("/data/geoip/geoip.mmdb")
            .max_size(134217728)
            .deprecated("Use geo-db instead", "geo-db");
        config.add_file("geo-db", geo_db, "City-level location database").path("/data/geo-db/geo.mmdb");
    }
};

}  // namespace fixture

namespace {

std::string conformance_dir() {
    if (const char* p = std::getenv("DOCUCONF_CONFORMANCE"); p && *p) return fs::path(p).parent_path().string();
    return std::string(DOCUCONF_SOURCE_DIR) + "/../docuconf-go/conformance";
}

std::string find_cli() {
    if (const char* c = std::getenv("DOCUCONF_CLI"); c && *c) return c;
    if (std::system("command -v docuconf >/dev/null 2>&1") == 0) return "docuconf";
    return "";
}

bool required() {
    const char* r = std::getenv("DOCUCONF_REQUIRE_CONFORMANCE");
    return r && std::string(r) == "1";
}

TEST(ExportFixture, DeclaresAndChecks) {
    fixture::Fixture f;
    EXPECT_NO_THROW(f.config.check());
}

TEST(ExportFixture, MatchesTheGoldenContract) {
    std::string golden = conformance_dir() + "/export/golden.cue";
    std::string cli = find_cli();
    if (!fs::exists(golden) || cli.empty()) {
        std::string why = !fs::exists(golden) ? "golden contract not found at " + golden
                                              : "docuconf CLI not found (set DOCUCONF_CLI)";
        if (required()) FAIL() << why;
        GTEST_SKIP() << why;
    }
    fixture::Fixture f;
    testutil::TempDir dir;
    std::string exported = dir.str() + "/exported.cue";
    f.config.write_contract(exported);
    std::string cmd = "'" + cli + "' conformance export --golden '" + golden + "' '" + exported + "' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    ASSERT_NE(p, nullptr);
    std::string out;
    std::array<char, 4096> buf{};
    while (std::size_t n = fread(buf.data(), 1, buf.size(), p)) out.append(buf.data(), n);
    int rc = pclose(p);
    // Exactly the reload differences, one line each, and nothing else.
    std::vector<std::string> lines;
    std::istringstream ss(out);
    for (std::string line; std::getline(ss, line);)
        if (!line.empty()) lines.push_back(line);
    std::vector<std::string> unexpected;
    int reload = 0;
    for (const auto& l : lines) {
        if (l.rfind("files.serving-tls.reload: golden \"watch\", exported \"restart\"", 0) == 0 ||
            l.rfind("files.settings.reload: golden \"watch\", exported \"restart\"", 0) == 0)
            ++reload;
        else if (l.rfind("docuconf conformance: ", 0) != 0 || l.find(": 2 differences") == std::string::npos)
            unexpected.push_back(l);
    }
    EXPECT_TRUE(WIFEXITED(rc) && WEXITSTATUS(rc) != 0) << out;  // it reports the differences
    EXPECT_EQ(reload, 2) << out;
    EXPECT_TRUE(unexpected.empty()) << out;
}

}  // namespace
