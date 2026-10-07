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
    env["DOCUCONF_TERMINATION_LOG"] = log;  // read from the env map, not the process
    auto e = load_fails(g, env);
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

TEST(Declaration, RejectsMistakes) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    int a = 0, b = 0;
    std::string c, e, s;
    d.add_var("bad_name", a, "Lowercase name");
    d.add_var("SHORT", b, "abc");
    d.add_var("RANGE", c, "Default outside its own constraints").pattern("^[a-z]+$").default_val("ABC");
    d.add_var("LOOKAROUND", e, "A pattern RE2 cannot compile").pattern("^(?=a)");
    d.add_var("SECRET_DEFAULT", s, "A secret with a default").secret().default_val("x");
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
    d.add_var("KS_PASSWORD", pw, "Keystore password, not secret").default_val("");
    d.add_var("CA_FILE", path_var, "Clashes with a pathEnv").default_val("");
    d.add_file("one", t1, "First text file").path("/etc/svc/a.txt").reload("watch");
    d.add_file("two", t2, "Second text file").path("/etc/svc/b.txt").dns_names({"x"});
    d.add_file("ca", ca, "Hides the system trust store").path("/etc/ssl/certs/private.pem").path_env("CA_FILE");
    d.add_file("ks", ks, "Keystore with a plain password").path("/etc/ks/ks.p12").password_var("KS_PASSWORD");
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
    d.add_var("ENABLE_NEW_CHECKOUT", on, "Turns on the new checkout").default_val(false);
    d.check();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("feature flag"), std::string::npos);
}

TEST(Declaration, RejectsCli11ConfigFiles) {
    CLI::App app;
    app.set_config("--config");
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
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

struct Run {
    int code = -1;
    bool exited = false;
    std::string out, err;
};

Run run(docuconf::Declaration& d, std::vector<const char*> argv) {
    Run r;
    std::ostringstream err;
    testing::internal::CaptureStdout();
    r.exited = d.parse_or_exit(static_cast<int>(argv.size()), argv.data(), r.code, err);
    r.out = testing::internal::GetCapturedStdout();
    r.err = err.str();
    return r;
}

TEST(Cli11, VariablesAreEnvironmentOnlyByDefault) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    std::string name;
    std::vector<std::string> tags;
    d.add_var("SVC_PORT", port, "HTTP listen port").range(1, 65535).default_val(8080);
    d.add_var("SVC_NAME", name, "Display name").default_val("svc");
    d.add_var("SVC_TAGS", tags, "Tags, comma separated").default_val(std::vector<std::string>{});
    EnvGuard env;
    env.set("SVC_PORT", "9090");
    env.set("SVC_NAME", "");  // an empty value is present for a string
    env.set("SVC_TAGS", "a,,b");
    std::vector<const char*> argv = {"svc"};
    d.parse(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(port, 9090);
    EXPECT_EQ(name, "");
    EXPECT_EQ(tags, (std::vector<std::string>{"a", "", "b"}));
    // No command-line option exists, so nothing can override the environment
    // the platform validated.
    EXPECT_EQ(app.get_option_no_throw("--svc-port"), nullptr);
    CLI::App app2{"svc"};
    docuconf::Declaration d2{app2, "svc"};
    d2.add_var("SVC_PORT", port, "HTTP listen port").default_val(8080);
    auto r = run(d2, {"svc", "--svc-port", "1"});
    EXPECT_TRUE(r.exited);
    EXPECT_NE(r.code, 0);
}

TEST(Cli11, FlagIsOptInWinsOverTheEnvironmentAndIsChecked) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("SVC_PORT", port, "HTTP listen port").range(1, 65535).default_val(8080).flag();
    EnvGuard env;
    env.set("SVC_PORT", "9090");
    auto r = run(d, {"svc", "--svc-port", "0"});
    EXPECT_TRUE(r.exited);
    EXPECT_EQ(r.code, 1);
    EXPECT_NE(r.err.find("SVC_PORT (--svc-port): 0 is below min 1 (out_of_range)"), std::string::npos) << r.err;

    CLI::App app2{"svc"};
    docuconf::Declaration d2{app2, "svc"};
    d2.add_var("SVC_PORT", port, "HTTP listen port").range(1, 65535).default_val(8080).flag("-p,--port");
    r = run(d2, {"svc", "-p", "7070"});
    EXPECT_FALSE(r.exited) << r.err;
    EXPECT_EQ(port, 7070);
}

TEST(Cli11, BoolFlagsBehaveLikeCli11Flags) {
    for (auto [args, want] : std::vector<std::pair<std::vector<const char*>, bool>>{
             {{"svc", "--debug"}, true},
             {{"svc", "--no-debug"}, false},
             {{"svc", "--debug=false"}, false},
             {{"svc"}, false}}) {
        CLI::App app{"svc"};
        docuconf::Declaration d{app, "svc"};
        bool debug = true;
        d.add_var("DEBUG", debug, "Serve the debug endpoints").default_val(false).flag();
        auto r = run(d, args);
        EXPECT_FALSE(r.exited) << r.err;
        EXPECT_EQ(debug, want) << args.size();
    }
}

TEST(Cli11, ListFlagsTakeSeveralArguments) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    std::vector<std::string> origins;
    std::vector<std::uint16_t> ports;
    d.add_var("ORIGINS", origins, "Allowed origins").flag();
    d.add_var("PORTS", ports, "Extra ports").default_val({}).flag();
    auto r = run(d, {"svc", "--origins", "a", "b", "--origins", "c,d", "--ports", "1", "2"});
    EXPECT_FALSE(r.exited) << r.err;
    EXPECT_EQ(origins, (std::vector<std::string>{"a", "b", "c", "d"}));
    EXPECT_EQ(ports, (std::vector<std::uint16_t>{1, 2}));
    CLI::App app2{"svc"};
    docuconf::Declaration d2{app2, "svc"};
    d2.add_var("PORTS", ports, "Extra ports").flag();
    r = run(d2, {"svc", "--ports", "1", "70000"});
    EXPECT_EQ(r.code, 1);
    EXPECT_NE(r.err.find("PORTS (--ports): "), std::string::npos) << r.err;
}

TEST(Cli11, SecretCannotBeAFlag) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    std::string url;
    d.add_var("DATABASE_URL", url, "Primary database").secret().flag();
    auto r = run(d, {"svc"});
    EXPECT_EQ(r.code, 2);
    EXPECT_NE(r.err.find("DATABASE_URL: a secret cannot be a command-line flag"), std::string::npos) << r.err;
}

TEST(Cli11, ClashesAreDeclarationErrorsNotCrashes) {
    CLI::App app{"svc"};
    int existing = 0, port = 0, twice = 0;
    app.add_option("--port", existing, "Existing CLI11 option")->envname("PORT");
    docuconf::Declaration d{app, "svc"};
    d.add_var("PORT", port, "HTTP listen port").default_val(8080).flag();
    d.add_var("TWICE", twice, "Declared twice").default_val(1);
    d.add_var("TWICE", twice, "Declared twice").default_val(1);
    auto r = run(d, {"svc"});
    EXPECT_EQ(r.code, 2);
    EXPECT_NE(r.err.find("PORT: flag --port is already a CLI11 option"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("TWICE: declared twice"), std::string::npos) << r.err;
}

TEST(Cli11, GroupIsForwardedToTheFlag) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port").group("Server").default_val(8080).flag();
    EXPECT_EQ(app.get_option("--port")->get_group(), "Server");
}

TEST(Cli11, DeclaringAfterLoadIsAnError) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0, late = 0;
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
    d.load({});
    try {
        d.add_var("LATE", late, "Declared after parse");
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& e) {
        EXPECT_NE(std::string(e.what()).find("LATE: declared after the configuration was loaded"), std::string::npos)
            << e.what();
    }
}

TEST(Cli11, ExportToABadPathIsOneLine) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
    auto r = run(d, {"svc", "--docuconf-export", "/nonexistent/dir/c.cue"});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "docuconf: cannot write /nonexistent/dir/c.cue: No such file or directory\n");
}

TEST(Cli11, ExportWritesThroughARename) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
    testutil::TempDir dir;
    std::string path = dir.str() + "/contract.cue";
    auto r = run(d, {"svc", "--docuconf-export", path.c_str()});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(testutil::read(path), d.export_cue());
    EXPECT_FALSE(fs::exists(path + ".tmp-docuconf"));
}

TEST(Cli11, HelpListsTheEnvironmentAndFiles) {
    CLI::App app{"svc"};
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    std::string url;
    docuconf::TextFile license;
    d.add_var("SVC_PORT", port, "HTTP listen port").default_val(8080);
    d.add_var("DATABASE_URL", url, "Primary database").secret().schemes({"postgres"});
    d.add_file("license", license, "License key").path("/etc/svc/license.key").path_env("LICENSE_FILE").required();
    auto r = run(d, {"svc", "--help"});
    EXPECT_EQ(r.code, 0);
    const std::string& help = r.out;
    EXPECT_NE(help.find("Environment variables:"), std::string::npos) << help;
    EXPECT_NE(help.find("SVC_PORT      HTTP listen port [int, default \"8080\"]"), std::string::npos) << help;
    EXPECT_NE(help.find("DATABASE_URL  Primary database [url, REQUIRED, secret]"), std::string::npos) << help;
    EXPECT_NE(help.find("license       License key [text at /etc/svc/license.key, path from LICENSE_FILE, REQUIRED]"),
              std::string::npos)
        << help;
    EXPECT_EQ(help.find("--svc-port"), std::string::npos) << help;
    EXPECT_NE(help.find("--docuconf-export"), std::string::npos) << help;
}

TEST(Load, TypoedNamesGetAHintWithoutTheValue) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::vector<std::string> warnings;
    d.on_warning([&](const std::string& w) { warnings.push_back(w); });
    std::string url, level;
    int port = 0;
    d.add_var("DATABASE_URL", url, "Primary database").secret();
    d.add_var("LOG_LEVEL", level, "Log level").default_val("info");
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
    d.load({{"DATABASE_URL", "postgres://x"},
            {"DATABSE_URL", "postgres://u:hunter2@db"},
            {"LOG_LEVL", "debug"},
            {"HOME", "/root"},
            {"PATH", "/usr/bin"},
            {"PORTS", "1"},
            {"DOCUCONF_FILE_ROOT", "/x"}});
    std::string all;
    for (const auto& w : warnings) all += w + "\n";
    EXPECT_NE(all.find("docuconf: DATABSE_URL is set but not declared; did you mean DATABASE_URL?"), std::string::npos)
        << all;
    EXPECT_NE(all.find("docuconf: LOG_LEVL is set but not declared; did you mean LOG_LEVEL?"), std::string::npos) << all;
    EXPECT_NE(all.find("PORTS is set but not declared; did you mean PORT?"), std::string::npos) << all;
    EXPECT_EQ(all.find("hunter2"), std::string::npos) << all;
    EXPECT_EQ(all.find("HOME"), std::string::npos) << all;
    EXPECT_EQ(all.find("PATH"), std::string::npos) << all;
}

TEST(Load, IntegerErrorsSayBase10) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    int port = 0;
    d.add_var("PORT", port, "HTTP listen port").default_val(8080);
    try {
        d.load({{"PORT", "0x10"}});
        FAIL();
    } catch (const docuconf::ValidationError& e) {
        EXPECT_NE(std::string(e.what()).find("PORT: \"0x10\" is not a base-10 integer (invalid_type)"),
                  std::string::npos)
            << e.what();
    }
}

}  // namespace
