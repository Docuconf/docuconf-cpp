// Boot validation of variables: parsing, defaults, binding to C++ types,
// aggregate errors, secret redaction, the termination log, and the CLI11
// integration.
#include <gtest/gtest.h>

#include "test_util.hpp"

using docuconf::Code;
using testutil::Gateway;

namespace {

/// A valid file tree for the gateway under a DOCUCONF_FILE_ROOT.
struct Files {
    testutil::TempDir root;
    Files() {
        auto ca = testutil::make_cert({"EC", "upstream ca", {}, -3600, 3650L * 86400, true, nullptr});
        auto leaf = testutil::make_cert({"EC", "gateway", {"gateway.internal", "api.example.com"}, -3600,
                                         365L * 86400, false, nullptr});
        root.write("etc/gateway/tls/tls.crt", leaf.cert_pem());
        root.write("etc/gateway/tls/tls.key", leaf.key_pem());
        root.write("etc/gateway/routes/routes.yaml", "routes:\n  - match: /api\n    upstream: https://api.internal\n");
        root.write("etc/gateway/upstream-ca/ca.pem", ca.cert_pem());
        root.write("etc/gateway/partner/keystore.p12", testutil::make_pkcs12(leaf, "pw"));
        root.write("etc/gateway/license/license.key", "ABCDE-12345-FGHIJ-67890\n");
    }
};

docuconf::Env base_env(const Files& f) {
    return {{"DOCUCONF_FILE_ROOT", f.root.str()},
            {"POD_NAMESPACE", "prod"},
            {"PARTNER_KEYSTORE_PASSWORD", "pw"},
            {"DATABASE_URL", "postgres://app:s3cr3t-pa55@db:5432/app"},
            {"ALLOWED_ORIGINS", "https://a.example.com,https://b.example.com"}};
}

docuconf::ValidationError load_fails(Gateway& g, const docuconf::Env& env) {
    try {
        g.config.load(env);
    } catch (const docuconf::ValidationError& e) {
        return e;
    }
    ADD_FAILURE() << "load succeeded, expected violations";
    return docuconf::ValidationError({});
}

TEST(Load, BindsTypedValuesAndDefaults) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["SHARDS"] = "1;2;1023";
    env["EXTRA_PORTS"] = "8443,9090";
    env["REQUEST_TIMEOUT"] = "1m30s";
    env["LOG_LEVEL"] = "warn";
    env["MEMORY_LIMIT"] = "9223372036854775807";
    env["RATE_LIMITS"] = R"({"perMinute":600,"burst":50})";
    env["DEBUG"] = "TRUE";
    env["TRACE_SAMPLE_RATIO"] = "0.5";
    g.config.load(env);
    EXPECT_EQ(g.port, 8080);
    EXPECT_EQ(g.log_level, testutil::LogLevel::Warn);
    EXPECT_EQ(g.pod_namespace, "prod");
    EXPECT_EQ(*g.memory_limit, 9223372036854775807LL);
    EXPECT_EQ(g.rate_limits.per_minute, 600u);
    EXPECT_EQ(*g.rate_limits.burst, 50u);
    EXPECT_EQ(g.request_timeout, std::chrono::seconds(90));
    EXPECT_EQ(g.allowed_origins, (std::vector<std::string>{"https://a.example.com", "https://b.example.com"}));
    EXPECT_EQ(*g.extra_ports, (std::vector<std::uint16_t>{8443, 9090}));
    EXPECT_EQ(*g.shards, (std::vector<std::int64_t>{1, 2, 1023}));
    EXPECT_EQ(g.stripe_api_base, "https://api.stripe.com");
    EXPECT_DOUBLE_EQ(g.trace_sample_ratio, 0.5);
    EXPECT_TRUE(g.debug);
    EXPECT_FALSE(g.region.has_value());
    EXPECT_EQ(g.cache_ttl, std::chrono::minutes(5));
    EXPECT_EQ(g.cache_size, 1000u);
    EXPECT_TRUE(g.serving_tls.present);
    ASSERT_TRUE(g.routes.present);
    EXPECT_EQ(g.routes->routes.at(0).upstream, "https://api.internal");
    EXPECT_EQ(g.upstream_ca.certificates, 1u);
    EXPECT_TRUE(g.partner_keystore.present);
    EXPECT_EQ(g.license.content, "ABCDE-12345-FGHIJ-67890\n");
    EXPECT_FALSE(g.geoip.present);
}

TEST(Load, BadIntIsInvalidType) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["PORT"] = "80x";
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("PORT"), std::vector<Code>{Code::InvalidType});
}

TEST(Load, NarrowIntBeyondItsTypeIsOutOfRange) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["PORT"] = "70000";
    env["EXTRA_PORTS"] = "443,0";
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("PORT"), std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(e.codes_for("EXTRA_PORTS"), std::vector<Code>{Code::OutOfRange});
}

TEST(Load, MissingRequiredVariables) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env.erase("POD_NAMESPACE");
    env.erase("DATABASE_URL");
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("POD_NAMESPACE"), std::vector<Code>{Code::MissingRequired});
    EXPECT_EQ(e.codes_for("DATABASE_URL"), std::vector<Code>{Code::MissingRequired});
}

TEST(Load, EmptyMeansUnsetExceptForStrings) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["PORT"] = "";
    env["POD_NAMESPACE"] = "";
    env["LOG_LEVEL"] = "";
    g.config.load(env);
    EXPECT_EQ(g.port, 8080);
    EXPECT_EQ(g.pod_namespace, "");
    EXPECT_EQ(g.log_level, testutil::LogLevel::Info);
}

TEST(Load, SecretValuesAreNeverPrinted) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["DATABASE_URL"] = "mysql://app:hunter2-secret@db/app";
    env["METRICS_TOKEN"] = "short-secret";
    testutil::TempDir logdir;
    std::string log = logdir.str() + "/termination-log";
    ::setenv("DOCUCONF_TERMINATION_LOG", log.c_str(), 1);
    auto e = load_fails(g, env);
    ::unsetenv("DOCUCONF_TERMINATION_LOG");
    EXPECT_EQ(e.codes_for("DATABASE_URL"), std::vector<Code>{Code::InvalidScheme});
    EXPECT_EQ(e.codes_for("METRICS_TOKEN"), std::vector<Code>{Code::OutOfRange});
    std::string what = e.what();
    EXPECT_EQ(what.find("hunter2"), std::string::npos) << what;
    EXPECT_EQ(what.find("short-secret"), std::string::npos) << what;
    std::string written = testutil::read(log);
    EXPECT_NE(written.find("invalid_scheme"), std::string::npos);
    EXPECT_EQ(written.find("hunter2"), std::string::npos);
}

TEST(Load, UnresolvedInjectorReference) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["DATABASE_URL"] = "vault:secret/data/db#url";
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("DATABASE_URL"), std::vector<Code>{Code::InvalidType});
    EXPECT_EQ(std::string(e.what()).find("secret/data/db"), std::string::npos);
}

TEST(Load, EveryViolationIsReportedTogether) {
    Files f;
    f.root.write("etc/gateway/routes/routes.yaml", "routes: [\n");
    fs::remove(f.root.path / "etc/gateway/license/license.key");
    Gateway g;
    auto env = base_env(f);
    env.erase("POD_NAMESPACE");
    env["PORT"] = "0";
    env["LOG_LEVEL"] = "verbose";
    env["REQUEST_TIMEOUT"] = "10m";
    env["ALLOWED_ORIGINS"] = "";
    env["TRACE_SAMPLE_RATIO"] = "NaN";
    env["REGION"] = "EU-west-1";
    env["RATE_LIMITS"] = R"({"perMinute":0})";
    env["SHARDS"] = "1;x";
    auto e = load_fails(g, env);
    std::vector<std::pair<std::string, Code>> want = {
        {"POD_NAMESPACE", Code::MissingRequired},  {"PORT", Code::OutOfRange},
        {"LOG_LEVEL", Code::NotInEnum},            {"REQUEST_TIMEOUT", Code::OutOfRange},
        {"ALLOWED_ORIGINS", Code::MissingRequired}, {"TRACE_SAMPLE_RATIO", Code::InvalidType},
        {"REGION", Code::PatternMismatch},         {"RATE_LIMITS", Code::SchemaMismatch},
        {"SHARDS", Code::InvalidType},             {"routes", Code::FileMalformed},
        {"license", Code::FileMissing}};
    for (const auto& [input, code] : want) {
        EXPECT_EQ(e.codes_for(input), std::vector<Code>{code}) << input << "\n" << e.what();
    }
    EXPECT_EQ(e.violations().size(), want.size()) << e.what();
    // Nothing was bound.
    EXPECT_EQ(g.port, 0);
}

TEST(Load, DeprecatedVariableWarns) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["REGION"] = "eu-west-1";
    g.config.load(env);
    ASSERT_EQ(g.warnings.size(), 1u);
    EXPECT_NE(g.warnings[0].find("REGION is deprecated"), std::string::npos);
    EXPECT_EQ(*g.region, "eu-west-1");
}

TEST(Load, ValuesAreNeverTrimmed) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["PORT"] = " 8080";
    env["POD_NAMESPACE"] = " prod\n";
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("PORT"), std::vector<Code>{Code::InvalidType});
    EXPECT_TRUE(e.codes_for("POD_NAMESPACE").empty());
}

TEST(Load, FloatsIgnoreTheLocale) {
    Files f;
    Gateway g;
    auto env = base_env(f);
    env["TRACE_SAMPLE_RATIO"] = "0,5";
    auto e = load_fails(g, env);
    EXPECT_EQ(e.codes_for("TRACE_SAMPLE_RATIO"), std::vector<Code>{Code::InvalidType});
}

// ---- declaration checks ----

// SPEC §4.3: maxLength on url and json values, itemMinLength and
// itemMaxLength on the items of a string list, counted in code points.
struct Lengths {
    CLI::App app;
    docuconf::Declaration config{app, "lengths"};
    std::optional<std::string> callback, db_url, name;
    std::optional<testutil::RateLimits> limits;
    std::optional<std::vector<std::string>> branches;
    Lengths() {
        config.add_var("CALLBACK", callback, "Where to report each run")->schemes({"https"})->max_length(24);
        config.add_var("DB_URL", db_url, "Database connection string")->url()->secret()->max_length(30);
        config.add_var("NAME", name, "Display name")->max_length(2);
        config.add_var("LIMITS", limits, "Run limits as a JSON object")->max_length(18);
        config.add_var("BRANCHES", branches, "Branch codes")->item_min_length(2)->item_max_length(4);
    }
};

docuconf::ValidationError lengths_fail(const docuconf::Env& env) {
    Lengths l;
    try {
        l.config.load(env);
    } catch (const docuconf::ValidationError& e) {
        return e;
    }
    ADD_FAILURE() << "load succeeded, expected violations";
    return docuconf::ValidationError({});
}

TEST(Lengths, CountCodePointsNotBytes) {
    Lengths l;
    l.config.load({{"CALLBACK", "https://例え.jp/日本語の道/一二三四"},
                   {"NAME", "日本"},
                   {"LIMITS", R"({"perMinute":600})"},
                   {"BRANCHES", "ZÜ01,日本,😀😀"}});
    EXPECT_EQ(*l.name, "日本");
    EXPECT_EQ(*l.branches, (std::vector<std::string>{"ZÜ01", "日本", "😀😀"}));
    EXPECT_EQ(l.limits->per_minute, 600u);
    EXPECT_EQ(lengths_fail({{"NAME", "日本語"}}).codes_for("NAME"), std::vector<Code>{Code::OutOfRange});
}

TEST(Lengths, UrlMaxLength) {
    EXPECT_EQ(lengths_fail({{"CALLBACK", "https://a.example/runs/42"}}).codes_for("CALLBACK"),
              std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(lengths_fail({{"CALLBACK", "https://例え.jp/日本語の道/一二三四五"}}).codes_for("CALLBACK"),
              std::vector<Code>{Code::OutOfRange});
    // A too-long secret reports its length, never its value.
    auto e = lengths_fail({{"DB_URL", "postgres://app:s3cr3t@db:5432/app"}});
    EXPECT_EQ(e.codes_for("DB_URL"), std::vector<Code>{Code::OutOfRange});
    std::string what = e.what();
    EXPECT_NE(what.find("33 characters"), std::string::npos) << what;
    EXPECT_EQ(what.find("s3cr3t"), std::string::npos) << what;
}

TEST(Lengths, JsonMaxLengthMeasuresTheValueAsReceived) {
    // 19 characters with the whitespace, 16 without.
    EXPECT_EQ(lengths_fail({{"LIMITS", R"({ "perMinute": 60 })"}}).codes_for("LIMITS"),
              std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(lengths_fail({{"LIMITS", "{"}}).codes_for("LIMITS"), std::vector<Code>{Code::InvalidType});
}

TEST(Lengths, ItemLengths) {
    EXPECT_EQ(lengths_fail({{"BRANCHES", "BE,ZÜRICH"}}).codes_for("BRANCHES"), std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(lengths_fail({{"BRANCHES", "BE,B"}}).codes_for("BRANCHES"), std::vector<Code>{Code::OutOfRange});
}

TEST(Lengths, ExportedAndVetted) {
    Lengths l;
    auto j = l.config.export_json();
    EXPECT_EQ(j["vars"]["CALLBACK"]["maxLength"], 24);
    EXPECT_EQ(j["vars"]["LIMITS"]["maxLength"], 18);
    EXPECT_EQ(j["vars"]["BRANCHES"]["itemMinLength"], 2);
    EXPECT_EQ(j["vars"]["BRANCHES"]["itemMaxLength"], 4);
    std::optional<std::string> err;
    try {
        err = testutil::cue_vet(l.config.export_cue());
    } catch (const std::runtime_error& e) {
        GTEST_SKIP() << e.what();
    }
    EXPECT_FALSE(err) << *err;
}

TEST(Lengths, DeclarationErrors) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::vector<std::string> a, b;
    std::vector<std::int64_t> ints;
    std::string c;
    int n = 0;
    d.add_var("A", a, "Min above max")->item_min_length(5)->item_max_length(4);
    d.add_var("B", b, "Default item too long")->item_max_length(2)->default_val(std::vector<std::string>{"ok", "ZÜ01"});
    d.add_var("C", c, "Default url too long")->url()->max_length(10)->default_val("https://example.com");
    d.add_var("INTS", ints, "Item lengths on ints")->item_max_length(3);
    d.add_var("N", n, "Max length on an int")->max_length(3);
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("A: itemMinLength is above itemMaxLength"), std::string::npos) << what;
        EXPECT_NE(what.find("B: default"), std::string::npos) << what;
        EXPECT_NE(what.find("C: default"), std::string::npos) << what;
        EXPECT_NE(what.find("INTS: itemMinLength and itemMaxLength only apply to a list of strings"),
                  std::string::npos)
            << what;
        EXPECT_NE(what.find("N: maxLength does not apply to a int variable"), std::string::npos) << what;
    }
}

docuconf::Contract lengths_contract(const std::string& vars) {
    auto j = nlohmann::json::parse(vars);
    j["apiVersion"] = "docuconf.dev/v1alpha1";
    j["kind"] = "ConfigContract";
    return docuconf::Contract::from_json(j);
}

TEST(Lengths, ContractFirst) {
    auto contract = lengths_contract(std::string(R"({"vars": {
        "LIMITS": {"type": "json", "description": "Run limits", "maxLength": 16},
        "BRANCHES": {"type": "list", "description": "Branch codes", "items": "string", "encoding": "indexed",
                     "itemMinLength": 2, "itemMaxLength": 4},
        "CODES": {"type": "list", "description": "Codes as JSON", "items": "string", "encoding": "json",
                  "itemMaxLength": 4}}})"));
    auto values = contract.load({{"LIMITS", R"({"n":"日本語の道路xy"})"}, {"BRANCHES__0", "ZÜ01"}, {"CODES", R"(["😀😀😀😀"])"}});
    EXPECT_EQ(values.to_json()["BRANCHES"], nlohmann::json::parse(R"(["ZÜ01"])"));
    try {
        contract.load({{"LIMITS", R"({"max":123456789})"}, {"BRANCHES__0", "BE"}, {"BRANCHES__1", "GENEVA"},
                       {"CODES", R"(["BE","GENEVA"])"}});
        FAIL() << "expected violations";
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("LIMITS"), std::vector<Code>{Code::OutOfRange});
        EXPECT_EQ(e.codes_for("BRANCHES"), std::vector<Code>{Code::OutOfRange});
        EXPECT_EQ(e.codes_for("CODES"), std::vector<Code>{Code::OutOfRange});
    }
    // A json default is measured as compact JSON: {"n":"日本語の道路"} is 14.
    EXPECT_THROW(lengths_contract(std::string(
                     R"({"vars": {"L": {"type": "json", "description": "Run limits", "maxLength": 13,
                         "default": {"n": "日本語の道路"}}}})")),
                 docuconf::DeclarationError);
    EXPECT_NO_THROW(lengths_contract(std::string(
        R"({"vars": {"L": {"type": "json", "description": "Run limits", "maxLength": 14,
            "default": {"n": "日本語の道路"}}}})")));
    EXPECT_THROW(lengths_contract(std::string(
                     R"({"vars": {"L": {"type": "list", "description": "Some ints", "items": "int",
                         "itemMaxLength": 4}}})")),
                 docuconf::DeclarationError);
}

TEST(Declaration, RejectsMistakes) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    int a = 0, b = 0;
    std::string c, e, s;
    d.add_var("bad_name", a, "Lowercase name");
    d.add_var("SHORT", b, "abc");
    d.add_var("RANGE", c, "Default outside its own constraints")->pattern("^[a-z]+$")->default_val("ABC");
    d.add_var("LOOKAROUND", e, "A pattern RE2 cannot compile")->pattern("^(?=a)");
    d.add_var("SECRET_DEFAULT", s, "A secret with a default")->secret()->default_val("x");
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("bad_name: variable name must match"), std::string::npos) << what;
        EXPECT_NE(what.find("SHORT: description must be at least 5 characters"), std::string::npos) << what;
        EXPECT_NE(what.find("RANGE: default \"ABC\" does not match pattern"), std::string::npos) << what;
        EXPECT_NE(what.find("LOOKAROUND: pattern"), std::string::npos) << what;
        EXPECT_NE(what.find("SECRET_DEFAULT: a secret must not have a default"), std::string::npos) << what;
    }
}

TEST(Declaration, RejectsBadFileInputs) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::string pw, path_var;
    docuconf::TextFile t1, t2;
    docuconf::CaBundle ca;
    docuconf::Keystore ks;
    d.add_var("KS_PASSWORD", pw, "Keystore password, not secret")->default_val("");
    d.add_var("CA_FILE", path_var, "Clashes with a pathEnv")->default_val("");
    d.add_file("one", t1, "First text file")->path("/etc/svc/a.txt")->reload("watch");
    d.add_file("two", t2, "Second text file")->path("/etc/svc/b.txt")->dns_names({"x"});
    d.add_file("ca", ca, "Hides the system trust store")->path("/etc/ssl/certs/private.pem")->path_env("CA_FILE");
    d.add_file("ks", ks, "Keystore with a plain password")->path("/etc/ks/ks.p12")->password_var("KS_PASSWORD");
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("one: reload \"watch\" is not supported"), std::string::npos) << what;
        EXPECT_NE(what.find("two: dnsNames does not apply to a text file"), std::string::npos) << what;
        EXPECT_NE(what.find("two: shares its mount directory /etc/svc with one"), std::string::npos) << what;
        EXPECT_NE(what.find("ca: would be mounted at /etc/ssl/certs"), std::string::npos) << what;
        EXPECT_NE(what.find("ca: pathEnv CA_FILE must not also be a declared variable"), std::string::npos) << what;
        EXPECT_NE(what.find("ks: passwordVar KS_PASSWORD must be a secret variable"), std::string::npos) << what;
    }
}

TEST(Declaration, WarnsOnFeatureFlagNames) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::vector<std::string> warnings;
    d.on_warning([&](const std::string& w) { warnings.push_back(w); });
    bool on = false;
    d.add_var("ENABLE_NEW_CHECKOUT", on, "Turns on the new checkout")->default_val(false);
    d.check();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("feature flag"), std::string::npos);
}

TEST(Declaration, RejectsCli11ConfigFiles) {
    CLI::App app;
    app.set_config("--config");
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port")->default_val(8080);
    EXPECT_THROW(d.check(), docuconf::DeclarationError);
}

// ---- CLI11 integration ----

struct EnvGuard {
    std::vector<std::string> names;
    void set(const std::string& k, const std::string& v) {
        ::setenv(k.c_str(), v.c_str(), 1);
        names.push_back(k);
    }
    ~EnvGuard() {
        for (const auto& n : names) ::unsetenv(n.c_str());
    }
};

TEST(Cli11, ReadsTheEnvironmentThroughEnvname) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    std::string name;
    std::vector<std::string> tags;
    d.add_var("SVC_PORT", port, "HTTP listen port")->range(1, 65535)->default_val(8080);
    d.add_var("SVC_NAME", name, "Display name")->default_val("svc");
    d.add_var("SVC_TAGS", tags, "Tags, comma separated")->default_val(std::vector<std::string>{});
    EnvGuard env;
    env.set("SVC_PORT", "9090");
    env.set("SVC_NAME", "");  // CLI11 skips an empty value; docuconf keeps it for a string
    env.set("SVC_TAGS", "a,,b");
    std::vector<const char*> argv = {"svc"};
    d.parse(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(port, 9090);
    EXPECT_EQ(name, "");
    EXPECT_EQ(tags, (std::vector<std::string>{"a", "", "b"}));
    EXPECT_EQ(d.var_specs()[0].name, "SVC_PORT");
    EXPECT_EQ(app.get_option("--svc-port")->get_envname(), "SVC_PORT");
}

TEST(Cli11, CommandLineOverridesTheEnvironmentAndIsChecked) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("SVC_PORT", port, "HTTP listen port")->range(1, 65535)->default_val(8080);
    EnvGuard env;
    env.set("SVC_PORT", "9090");
    std::vector<const char*> argv = {"svc", "--svc-port", "0"};
    int code = 0;
    std::ostringstream err;
    EXPECT_TRUE(d.parse_or_exit(static_cast<int>(argv.size()), argv.data(), code, err));
    EXPECT_EQ(code, 1);
    EXPECT_NE(err.str().find("SVC_PORT: 0 is below min 1 (out_of_range)"), std::string::npos) << err.str();
}

TEST(Cli11, HelpListsEnvironmentNames) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("SVC_PORT", port, "HTTP listen port")->default_val(8080);
    std::string help = app.help();
    EXPECT_NE(help.find("--svc-port"), std::string::npos) << help;
    EXPECT_NE(help.find("SVC_PORT"), std::string::npos) << help;
    EXPECT_NE(help.find("--docuconf-export"), std::string::npos) << help;
}

}  // namespace
