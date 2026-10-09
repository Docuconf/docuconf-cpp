// Boot checks of file inputs, with real files and certificates generated
// in the test.
#include <gtest/gtest.h>
#include <unistd.h>

#include "test_util.hpp"

using docuconf::Code;
using testutil::CertOptions;
using testutil::make_cert;
using testutil::TempDir;

namespace {

struct Svc {
    CLI::App app{"svc"};
    docuconf::Declaration config{app, "svc"};
};

std::vector<Code> codes(Svc& s, const docuconf::Env& env, const std::string& input) {
    try {
        s.config.load(env);
    } catch (const docuconf::ValidationError& e) {
        return e.codes_for(input);
    }
    return {};
}

docuconf::Env root_env(const TempDir& d) { return {{"DOCUCONF_FILE_ROOT", d.str()}}; }

// ---- TLS ----

struct Tls {
    Svc s;
    docuconf::TlsKeyPair tls;
    docuconf::File* file;
    Tls() {
        file = &s.config.add_file("serving-tls", tls, "Certificate served over HTTPS")
                   .path("/etc/svc/tls")
                   .required()
                   .dns_names({"svc.internal", "api.example.com"})
                   .min_remaining("720h");
    }
};

void write_pair(const TempDir& d, const testutil::KeyCert& kc, const std::string& extra_chain = "") {
    d.write("etc/svc/tls/tls.crt", kc.cert_pem() + extra_chain);
    d.write("etc/svc/tls/tls.key", kc.key_pem());
}

TEST(Tls, ValidPair) {
    TempDir d;
    write_pair(d, make_cert({"EC", "svc", {"svc.internal", "api.example.com"}}));
    Tls t;
    t.s.config.load(root_env(d));
    EXPECT_TRUE(t.tls.present);
    EXPECT_EQ(t.tls.dir, d.str() + "/etc/svc/tls");
    EXPECT_NE(t.tls.key_pem.find("PRIVATE KEY"), std::string::npos);
}

TEST(Tls, WildcardCoversOneLabel) {
    TempDir d;
    write_pair(d, make_cert({"RSA", "svc", {"svc.internal", "*.example.com"}}));
    Tls t;
    EXPECT_TRUE(codes(t.s, root_env(d), "serving-tls").empty());
    // A wildcard does not cover a second label.
    Tls t2;
    t2.file->dns_names({"svc.internal", "a.api.example.com"});
    EXPECT_EQ(codes(t2.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateNameMismatch});
}

TEST(Tls, ExpiringWithinMinRemaining) {
    TempDir d;
    write_pair(d, make_cert({"EC", "svc", {"svc.internal", "api.example.com"}, -3600, 10L * 86400}));
    Tls t;
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateExpiring});
}

TEST(Tls, ExpiredOrNotYetValid) {
    TempDir d;
    write_pair(d, make_cert({"EC", "svc", {"svc.internal", "api.example.com"}, -7200, -3600}));
    Tls t;
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateInvalid});
    TempDir d2;
    write_pair(d2, make_cert({"EC", "svc", {"svc.internal", "api.example.com"}, 3600, 90L * 86400}));
    Tls t2;
    EXPECT_EQ(codes(t2.s, root_env(d2), "serving-tls"), std::vector<Code>{Code::CertificateInvalid});
}

TEST(Tls, DnsMismatch) {
    TempDir d;
    write_pair(d, make_cert({"EC", "svc", {"svc.internal", "other.example.com"}}));
    Tls t;
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateNameMismatch});
}

TEST(Tls, KeyMismatch) {
    TempDir d;
    auto a = make_cert({"EC", "svc", {"svc.internal", "api.example.com"}});
    auto b = make_cert({"EC", "svc", {"svc.internal", "api.example.com"}});
    d.write("etc/svc/tls/tls.crt", a.cert_pem());
    d.write("etc/svc/tls/tls.key", b.key_pem());
    Tls t;
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::KeyMismatch});
}

TEST(Tls, KeyAlgorithmNotAllowed) {
    TempDir d;
    write_pair(d, make_cert({"Ed25519", "svc", {"svc.internal", "api.example.com"}}));
    Tls t;
    t.file->key_algorithms({"ECDSA", "RSA"});
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateInvalid});
}

TEST(Tls, RequireCaChains) {
    auto root = make_cert({"EC", "root", {}, -3600, 3650L * 86400, true});
    auto inter = make_cert({"EC", "intermediate", {}, -3600, 3650L * 86400, true, &root});
    auto leaf = make_cert({"EC", "svc", {"svc.internal", "api.example.com"}, -3600, 365L * 86400, false, &inter});
    auto stranger = make_cert({"EC", "other root", {}, -3600, 3650L * 86400, true});
    {
        TempDir d;
        write_pair(d, leaf, inter.cert_pem());
        d.write("etc/svc/tls/ca.crt", root.cert_pem());
        Tls t;
        t.file->require_ca();
        EXPECT_TRUE(codes(t.s, root_env(d), "serving-tls").empty());
        EXPECT_FALSE(t.tls.ca_pem.empty());
    }
    {
        TempDir d;
        write_pair(d, leaf, inter.cert_pem());
        d.write("etc/svc/tls/ca.crt", stranger.cert_pem());
        Tls t;
        t.file->require_ca();
        EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateInvalid});
    }
    {
        TempDir d;
        write_pair(d, leaf, inter.cert_pem());
        Tls t;
        t.file->require_ca();
        EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::FileMissing});
    }
}

TEST(Tls, MissingRequiredDirectory) {
    TempDir d;
    Tls t;
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::FileMissing});
}

TEST(Tls, GarbageCertificate) {
    TempDir d;
    d.write("etc/svc/tls/tls.crt", "not a certificate");
    d.write("etc/svc/tls/tls.key", make_cert({}).key_pem());
    Tls t;
    // No PEM certificate at all is file_malformed (SPEC §11.2 item 5).
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::FileMalformed});
    // A PEM block that does not parse is certificate_invalid.
    d.write("etc/svc/tls/tls.crt", "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n");
    EXPECT_EQ(codes(t.s, root_env(d), "serving-tls"), std::vector<Code>{Code::CertificateInvalid});
}

// ---- config files ----

struct Limits {
    int max = 0;
    static json json_schema() {
        return json::parse(R"({"type":"object","properties":{"max":{"type":"integer","minimum":1}},
                              "required":["max"],"additionalProperties":false})");
    }
};
void from_json(const json& j, Limits& l) { l.max = j.at("max").get<int>(); }

// Accepted by the schema, but max must fit an int8 in the app's type.
struct Tiny {
    std::int8_t max = 0;
    static json json_schema() { return json::parse(R"({"type":"object"})"); }
};
void from_json(const json& j, Tiny& t) {
    int v = j.at("max").get<int>();
    if (v > 127) throw std::out_of_range("max does not fit in an int8");
    t.max = static_cast<std::int8_t>(v);
}

TEST(Config, ParsesEveryFormat) {
    for (const auto& [name, body] : std::vector<std::pair<std::string, std::string>>{
             {"limits.json", "\xEF\xBB\xBF{\"max\": 5}"},
             {"limits.yaml", "max: 5\n"},
             {"limits.yml", "# comment\nmax: 5\n"},
             {"limits.toml", "max = 5\n"}}) {
        TempDir d;
        d.write("etc/svc/" + name, body);
        Svc s;
        docuconf::ConfigFile<Limits> limits;
        s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/" + name).required();
        s.config.load(root_env(d));
        EXPECT_TRUE(limits.present) << name;
        EXPECT_EQ(limits->max, 5) << name;
    }
}

TEST(Config, Malformed) {
    TempDir d;
    d.write("etc/svc/limits.yaml", "max: [5\n");
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/limits.yaml");
    EXPECT_EQ(codes(s, root_env(d), "limits"), std::vector<Code>{Code::FileMalformed});
}

TEST(Config, SchemaViolation) {
    TempDir d;
    d.write("etc/svc/limits.json", R"({"max": 0, "extra": true})");
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/limits.json");
    EXPECT_EQ(codes(s, root_env(d), "limits"), std::vector<Code>{Code::SchemaMismatch});
}

TEST(Config, DoesNotBindToTheAppsType) {
    TempDir d;
    d.write("etc/svc/tiny.json", R"({"max": 300})");
    Svc s;
    docuconf::ConfigFile<Tiny> tiny;
    s.config.add_file("tiny", tiny, "Tiny limits for the service").path("/etc/svc/tiny.json");
    EXPECT_EQ(codes(s, root_env(d), "tiny"), std::vector<Code>{Code::SchemaMismatch});
}

TEST(Config, OptionalAndMissingIsAbsent) {
    TempDir d;
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/limits.json");
    s.config.load(root_env(d));
    EXPECT_FALSE(limits.present);
}

TEST(Config, PathEnvIsPrefixedWithTheFileRoot) {
    TempDir d;
    d.write("elsewhere/limits.toml", "max = 7\n");
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service")
        .path("/etc/svc/limits.toml")
        .path_env("LIMITS_FILE")
        .required();
    auto env = root_env(d);
    env["LIMITS_FILE"] = "/elsewhere/limits.toml";
    s.config.load(env);
    EXPECT_EQ(limits->max, 7);
    EXPECT_EQ(limits.path, d.str() + "/elsewhere/limits.toml");
}

TEST(Config, TooLarge) {
    TempDir d;
    d.write("etc/svc/limits.json", R"({"max": 5})");
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/limits.json").max_size(4);
    EXPECT_EQ(codes(s, root_env(d), "limits"), std::vector<Code>{Code::FileTooLarge});
}

TEST(Config, Unreadable) {
    if (::geteuid() == 0) GTEST_SKIP() << "root reads every file";
    TempDir d;
    std::string p = d.write("etc/svc/limits.json", R"({"max": 5})");
    fs::permissions(p, fs::perms::none);
    Svc s;
    docuconf::ConfigFile<Limits> limits;
    s.config.add_file("limits", limits, "Limits for the service").path("/etc/svc/limits.json");
    EXPECT_EQ(codes(s, root_env(d), "limits"), std::vector<Code>{Code::FileUnreadable});
}

// ---- CA bundles, keystores, text, binary ----

TEST(CaBundle, CountsCertificates) {
    auto a = make_cert({"EC", "a", {}, -3600, 86400 * 365L, true});
    auto b = make_cert({"EC", "b", {}, -3600, 86400 * 365L, true});
    TempDir d;
    d.write("etc/svc/ca/bundle.pem", a.cert_pem() + b.cert_pem());
    Svc s;
    docuconf::CaBundle ca;
    s.config.add_file("ca", ca, "Private CAs to trust").path("/etc/svc/ca/bundle.pem").min_certificates(3);
    EXPECT_EQ(codes(s, root_env(d), "ca"), std::vector<Code>{Code::FileMalformed});
    Svc s2;
    docuconf::CaBundle ca2;
    s2.config.add_file("ca", ca2, "Private CAs to trust").path("/etc/svc/ca/bundle.pem").min_certificates(2);
    s2.config.load(root_env(d));
    EXPECT_EQ(ca2.certificates, 2u);
}

struct Ks {
    Svc s;
    std::string password;
    docuconf::Keystore ks;
    explicit Ks(const std::string& format) {
        s.config.add_var("KS_PASSWORD", password, "Keystore password").secret();
        s.config.add_file("partner", ks, "Partner client keystore")
            .path("/etc/svc/partner/ks.bin")
            .format(format)
            .password_var("KS_PASSWORD")
            .required();
    }
};

TEST(Keystore, Pkcs12OpensWithItsPassword) {
    auto kc = make_cert({"RSA", "partner"});
    TempDir d;
    d.write("etc/svc/partner/ks.bin", testutil::make_pkcs12(kc, "changeit"));
    {
        Ks k("pkcs12");
        auto env = root_env(d);
        env["KS_PASSWORD"] = "changeit";
        k.s.config.load(env);
        EXPECT_TRUE(k.ks.present);
    }
    {
        Ks k("pkcs12");
        auto env = root_env(d);
        env["KS_PASSWORD"] = "wrong-password";
        try {
            k.s.config.load(env);
            FAIL();
        } catch (const docuconf::ValidationError& e) {
            EXPECT_EQ(e.codes_for("partner"), std::vector<Code>{Code::KeystoreUnreadable});
            EXPECT_EQ(std::string(e.what()).find("wrong-password"), std::string::npos);
        }
    }
    {
        TempDir bad;
        bad.write("etc/svc/partner/ks.bin", "garbage");
        Ks k("pkcs12");
        auto env = root_env(bad);
        env["KS_PASSWORD"] = "changeit";
        EXPECT_EQ(codes(k.s, env, "partner"), std::vector<Code>{Code::KeystoreUnreadable});
    }
}

TEST(Keystore, JksIntegrityDigest) {
    TempDir d;
    d.write("etc/svc/partner/ks.bin", testutil::make_jks("changeit"));
    {
        Ks k("jks");
        auto env = root_env(d);
        env["KS_PASSWORD"] = "changeit";
        k.s.config.load(env);
        EXPECT_TRUE(k.ks.present);
    }
    {
        Ks k("jks");
        auto env = root_env(d);
        env["KS_PASSWORD"] = "nope";
        EXPECT_EQ(codes(k.s, env, "partner"), std::vector<Code>{Code::KeystoreUnreadable});
    }
}

TEST(Text, PatternAndLength) {
    TempDir d;
    d.write("etc/svc/license/key", "ABCDE-12345\n");
    {
        Svc s;
        docuconf::TextFile t;
        s.config.add_file("license", t, "Licence key").path("/etc/svc/license/key").pattern("^[A-Z0-9]{5}-[0-9]{5}\\n?$");
        s.config.load(root_env(d));
        EXPECT_EQ(t.content, "ABCDE-12345\n");
    }
    {
        Svc s;
        docuconf::TextFile t;
        s.config.add_file("license", t, "Licence key").path("/etc/svc/license/key").pattern("^[0-9]+$");
        EXPECT_EQ(codes(s, root_env(d), "license"), std::vector<Code>{Code::PatternMismatch});
    }
    {
        Svc s;
        docuconf::TextFile t;
        s.config.add_file("license", t, "Licence key").path("/etc/svc/license/key").max_length(5);
        EXPECT_EQ(codes(s, root_env(d), "license"), std::vector<Code>{Code::OutOfRange});
    }
}

TEST(Binary, SizeOnly) {
    TempDir d;
    d.write("data/geo/db.mmdb", std::string("\x00\x01\x02", 3));
    Svc s;
    docuconf::BinaryFile b;
    s.config.add_file("geoip", b, "GeoIP database").path("/data/geo/db.mmdb").max_size(16).required();
    s.config.load(root_env(d));
    EXPECT_EQ(b.data.size(), 3u);
}

TEST(Files, MissingRequiredFile) {
    TempDir d;
    Svc s;
    docuconf::BinaryFile b;
    s.config.add_file("geoip", b, "GeoIP database").path("/data/geo/db.mmdb").required();
    EXPECT_EQ(codes(s, root_env(d), "geoip"), std::vector<Code>{Code::FileMissing});
}

}  // namespace
