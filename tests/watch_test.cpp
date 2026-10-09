// `reload: watch` (SPEC §4.6.2, §11.2 item 8): docuconf::Watched rereads a
// file input, or a contract's watched files and overlays, when they change,
// including the way Kubernetes changes a volume: by swapping a symlink.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "test_util.hpp"

using docuconf::Code;
using testutil::TempDir;

namespace {

// Publishes `files` into `dir` as the kubelet updates a ConfigMap or Secret
// volume: the files go into a new hidden directory, the `..data` symlink is
// swapped to it in one rename, and each visible file is a symlink through
// `..data`. The old directory is then removed.
class Volume {
public:
    explicit Volume(fs::path dir) : dir_(std::move(dir)) { fs::create_directories(dir_); }

    void publish(const std::map<std::string, std::string>& files) {
        std::string gen = "..2026_10_09_" + std::to_string(++generation_);
        fs::create_directories(dir_ / gen);
        for (const auto& [name, content] : files) std::ofstream(dir_ / gen / name, std::ios::binary) << content;
        fs::remove(dir_ / "..data_tmp");
        fs::create_directory_symlink(gen, dir_ / "..data_tmp");
        fs::rename(dir_ / "..data_tmp", dir_ / "..data");  // atomic, like the kubelet's swap
        for (const auto& [name, content] : files)
            if (!fs::is_symlink(dir_ / name)) fs::create_symlink(fs::path("..data") / name, dir_ / name);
        if (!old_.empty()) fs::remove_all(dir_ / old_);
        old_ = gen;
    }

private:
    fs::path dir_;
    int generation_ = 0;
    std::string old_;
};

struct Svc {
    CLI::App app{"svc"};
    docuconf::Declaration config{app, "svc"};
    std::vector<std::string> warnings;
    Svc() {
        config.on_warning([this](const std::string& w) { warnings.push_back(w); });
    }
};

docuconf::Env root_env(const TempDir& d) { return {{"DOCUCONF_FILE_ROOT", d.str()}}; }

TEST(Watch, ExportsReloadWatch) {
    Svc s;
    docuconf::Watched<docuconf::TextFile> motd;
    s.config.add_file("motd", motd, "Message of the day").path("/etc/svc/motd/motd.txt");
    EXPECT_NE(s.config.export_cue().find("reload: \"watch\""), std::string::npos) << s.config.export_cue();
}

TEST(Watch, WatchedTargetMustBeDeclaredWatch) {
    Svc s;
    docuconf::Watched<docuconf::TextFile> motd;
    s.config.add_file("motd", motd, "Message of the day").path("/etc/svc/motd/motd.txt").reload("restart");
    try {
        s.config.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& e) {
        EXPECT_NE(std::string(e.what()).find("motd: a docuconf::Watched target rereads the file"), std::string::npos)
            << e.what();
    }
}

TEST(Watch, BeforeLoadHoldsADefaultValue) {
    docuconf::Watched<docuconf::TextFile> motd;
    ASSERT_NE(motd.current(), nullptr);
    EXPECT_FALSE(motd.current()->present);
    EXPECT_EQ(motd.generation(), 0u);
    EXPECT_FALSE(motd.refresh());
}

TEST(Watch, KubernetesSymlinkSwap) {
    TempDir d;
    Volume vol(d.path / "etc/svc/motd");
    vol.publish({{"motd.txt", "hello"}});
    Svc s;
    docuconf::Watched<docuconf::TextFile> motd;
    s.config.add_file("motd", motd, "Message of the day").path("/etc/svc/motd/motd.txt").required();
    s.config.load(root_env(d));
    auto first = motd.current();
    EXPECT_EQ(first->content, "hello");
    EXPECT_EQ(motd.generation(), 1u);
    EXPECT_FALSE(motd.refresh());  // nothing changed

    vol.publish({{"motd.txt", "world"}});
    EXPECT_TRUE(motd.refresh());
    EXPECT_EQ(motd.current()->content, "world");
    EXPECT_EQ(motd.current()->path, d.str() + "/etc/svc/motd/motd.txt");
    EXPECT_EQ(motd.generation(), 2u);
    EXPECT_EQ(first->content, "hello");  // a value already handed out never changes
    EXPECT_TRUE(s.warnings.empty());
}

TEST(Watch, TlsKeyPairRotation) {
    TempDir d;
    Volume vol(d.path / "etc/svc/tls");
    auto one = testutil::make_cert({"EC", "svc", {"svc.internal"}});
    vol.publish({{"tls.crt", one.cert_pem()}, {"tls.key", one.key_pem()}});
    Svc s;
    docuconf::Watched<docuconf::TlsKeyPair> tls;
    s.config.add_file("serving-tls", tls, "Certificate served over HTTPS")
        .path("/etc/svc/tls")
        .required()
        .dns_names({"svc.internal"});
    s.config.load(root_env(d));
    EXPECT_EQ(tls.current()->certificate_pem, one.cert_pem());

    auto two = testutil::make_cert({"EC", "svc", {"svc.internal"}});
    vol.publish({{"tls.crt", two.cert_pem()}, {"tls.key", two.key_pem()}});
    EXPECT_TRUE(tls.refresh());
    EXPECT_EQ(tls.current()->certificate_pem, two.cert_pem());
    EXPECT_EQ(tls.current()->key_pem, two.key_pem());

    // A certificate that no longer covers the name is not used.
    auto wrong = testutil::make_cert({"EC", "svc", {"other.internal"}});
    vol.publish({{"tls.crt", wrong.cert_pem()}, {"tls.key", wrong.key_pem()}});
    EXPECT_FALSE(tls.refresh());
    EXPECT_EQ(tls.current()->certificate_pem, two.cert_pem());
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_NE(s.warnings[0].find("(certificate_name_mismatch)"), std::string::npos) << s.warnings[0];
    EXPECT_EQ(s.warnings[0].find("PRIVATE KEY"), std::string::npos) << s.warnings[0];
}

TEST(Watch, BadChangeKeepsThePreviousValue) {
    TempDir d;
    Volume vol(d.path / "etc/svc/licence");
    vol.publish({{"licence.key", "ABC-123"}});
    Svc s;
    docuconf::Watched<docuconf::TextFile> licence;
    s.config.add_file("licence", licence, "Licence key")
        .path("/etc/svc/licence/licence.key")
        .required()
        .pattern("^[A-Z0-9-]+$");
    s.config.load(root_env(d));

    vol.publish({{"licence.key", "not a licence"}});
    EXPECT_FALSE(licence.refresh());
    EXPECT_EQ(licence.current()->content, "ABC-123");
    EXPECT_EQ(licence.generation(), 1u);
    ASSERT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(s.warnings[0],
              "licence changed, but the new version was not loaded; keeping the previous one:\n"
              "  licence: does not match pattern ^[A-Z0-9-]+$ (pattern_mismatch)");
    // The same bad version is reported once.
    EXPECT_FALSE(licence.refresh());
    EXPECT_EQ(s.warnings.size(), 1u);

    // A required file that disappears is file_missing; the value stays.
    fs::remove(d.path / "etc/svc/licence/licence.key");
    EXPECT_FALSE(licence.refresh());
    EXPECT_EQ(licence.current()->content, "ABC-123");
    ASSERT_EQ(s.warnings.size(), 2u);
    EXPECT_NE(s.warnings[1].find("(file_missing)"), std::string::npos) << s.warnings[1];

    // The next good version is loaded.
    fs::create_symlink(fs::path("..data") / "licence.key", d.path / "etc/svc/licence/licence.key");
    vol.publish({{"licence.key", "XYZ-789"}});
    EXPECT_TRUE(licence.refresh());
    EXPECT_EQ(licence.current()->content, "XYZ-789");
    EXPECT_EQ(licence.generation(), 2u);
}

TEST(Watch, OptionalFileComesAndGoes) {
    TempDir d;
    Svc s;
    docuconf::Watched<docuconf::BinaryFile> blob;
    s.config.add_file("blob", blob, "Optional data blob").path("/data/blob/blob.bin");
    s.config.load(root_env(d));
    EXPECT_FALSE(blob.current()->present);
    d.write("data/blob/blob.bin", std::string("\x00\x01", 2));
    EXPECT_TRUE(blob.refresh());
    EXPECT_TRUE(blob.current()->present);
    EXPECT_EQ(blob.current()->data, std::string("\x00\x01", 2));
    fs::remove(d.path / "data/blob/blob.bin");
    EXPECT_TRUE(blob.refresh());
    EXPECT_FALSE(blob.current()->present);
    EXPECT_TRUE(s.warnings.empty());
}

struct Credentials {
    std::string token;
    static json json_schema() {
        return json::parse(R"({"type": "object", "required": ["token"], "additionalProperties": false,
            "properties": {"token": {"type": "string", "minLength": 8, "maxLength": 20}}})");
    }
};
inline void from_json(const json& j, Credentials& c) { j.at("token").get_to(c.token); }

TEST(Watch, WarningsNeverHoldSecretContent) {
    TempDir d;
    Volume vol(d.path / "etc/svc/creds");
    vol.publish({{"creds.json", R"({"token": "s3cr3t-first"})"}});
    Svc s;
    docuconf::Watched<docuconf::ConfigFile<Credentials>> creds;
    s.config.add_file("creds", creds, "Credentials for the search API")
        .path("/etc/svc/creds/creds.json")
        .required()
        .secret();
    s.config.load(root_env(d));
    EXPECT_EQ(creds.current()->value.token, "s3cr3t-first");

    vol.publish({{"creds.json", R"({"token": "s3cr3t-unterminated)"}});  // does not parse
    EXPECT_FALSE(creds.refresh());
    vol.publish({{"creds.json", R"({"token": "s3cr3t-far-too-long-for-the-schema"})"}});
    EXPECT_FALSE(creds.refresh());
    vol.publish({{"creds.json", R"({"token": ["s3cr3t-in-a-list"]})"}});
    EXPECT_FALSE(creds.refresh());
    EXPECT_EQ(creds.current()->value.token, "s3cr3t-first");
    ASSERT_EQ(s.warnings.size(), 3u);
    for (const auto& w : s.warnings) {
        EXPECT_EQ(w.find("s3cr3t"), std::string::npos) << w;
        EXPECT_EQ(w.rfind("creds changed", 0), 0u) << w;
    }
    EXPECT_NE(s.warnings[0].find("(file_malformed)"), std::string::npos) << s.warnings[0];
    EXPECT_NE(s.warnings[1].find("(schema_mismatch)"), std::string::npos) << s.warnings[1];
}

TEST(Watch, CurrentChecksAtMostOncePerInterval) {
    TempDir d;
    d.write("etc/svc/motd/motd.txt", "one");
    Svc s;
    docuconf::Watched<docuconf::TextFile> motd;
    s.config.add_file("motd", motd, "Message of the day").path("/etc/svc/motd/motd.txt");
    s.config.load(root_env(d));
    motd.check_interval(std::chrono::hours(1));
    d.write("etc/svc/motd/motd.txt", "two, a longer one");
    EXPECT_EQ(motd.current()->content, "one");  // not due for an hour
    motd.check_interval(std::chrono::seconds(0));
    EXPECT_EQ(motd.current()->content, "two, a longer one");
}

TEST(Watch, ReadersAndReloadsOnManyThreads) {
    TempDir d;
    Volume vol(d.path / "etc/svc/motd");
    vol.publish({{"motd.txt", "v0"}});
    Svc s;
    docuconf::Watched<docuconf::TextFile> motd;
    s.config.add_file("motd", motd, "Message of the day")
        .path("/etc/svc/motd/motd.txt")
        .required()
        .pattern("^v[0-9]+$");
    s.config.load(root_env(d));
    motd.check_interval(std::chrono::seconds(0));

    std::atomic<bool> done{false};
    std::atomic<int> bad{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i)
        readers.emplace_back([&] {
            int last = 0;
            while (!done) {
                auto v = motd.current();
                // Always a complete, checked value, and never an older one
                // than this thread has already seen.
                if (!v->present || v->content.size() < 2 || v->content[0] != 'v') {
                    ++bad;
                    continue;
                }
                int n = std::stoi(v->content.substr(1));
                if (n < last) ++bad;
                last = n;
                ++reads;
            }
        });
    const int kVersions = 50;
    for (int i = 1; i <= kVersions; ++i) {
        vol.publish({{"motd.txt", "v" + std::to_string(i)}});
        if (i % 10 == 0) vol.publish({{"motd.txt", "bad"}});  // fails its pattern: never seen
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    vol.publish({{"motd.txt", "v" + std::to_string(kVersions + 1)}});
    motd.refresh();
    done = true;
    for (auto& t : readers) t.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(motd.current()->content, "v" + std::to_string(kVersions + 1));
}

// ---- Contract-first mode ----

const char* kContract = R"({
  "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "catalog"},
  "vars": {
    "PAGE_SIZE": {"type": "int", "description": "Items per page", "configKey": "Catalog.PageSize",
                  "min": 1, "default": 10},
    "MODE": {"type": "string", "description": "Serving mode", "configKey": "Catalog.Mode", "default": "a"}
  },
  "overlays": {
    "platform": {"format": "yaml", "path": "/app/config/platform.yaml", "keySeparator": ".", "reload": "watch"},
    "fixed": {"format": "json", "path": "/app/fixed/fixed.json", "keySeparator": "."}
  },
  "files": {
    "motd": {"type": "text", "description": "Message of the day", "path": "/etc/app/motd/motd.txt",
             "reload": "watch", "pattern": "^[a-z ]+$"},
    "banner": {"type": "text", "description": "Banner, read once", "path": "/etc/app/banner/banner.txt"}
  }
})";

TEST(WatchContract, ReloadsWatchedInputsOnly) {
    TempDir d;
    d.write("app/config/platform.yaml", "Catalog:\n  PageSize: 20\n");
    d.write("app/fixed/fixed.json", R"({"Catalog": {"Mode": "b"}})");
    d.write("etc/app/motd/motd.txt", "hello");
    d.write("etc/app/banner/banner.txt", "first banner");
    auto c = docuconf::Contract::from_json(std::string(kContract));
    std::vector<std::string> warnings;
    c.on_warning([&](const std::string& w) { warnings.push_back(w); });
    auto values = c.watch(root_env(d));
    EXPECT_EQ(values.current()->get("PAGE_SIZE")->as_int(), 20);
    EXPECT_EQ(values.current()->get("MODE")->as_string(), "b");
    EXPECT_EQ(values.current()->file("motd")->content, "hello");

    // A restart input is read once: changing it alone triggers nothing, and
    // a reload keeps its boot value.
    d.write("etc/app/banner/banner.txt", "second banner");
    d.write("app/fixed/fixed.json", R"({"Catalog": {"Mode": "c"}})");
    EXPECT_FALSE(values.refresh());

    d.write("app/config/platform.yaml", "Catalog:\n  PageSize: 40\n");
    d.write("etc/app/motd/motd.txt", "hello again");
    EXPECT_TRUE(values.refresh());
    auto v = values.current();
    EXPECT_EQ(v->get("PAGE_SIZE")->as_int(), 40);
    EXPECT_EQ(v->file("motd")->content, "hello again");
    EXPECT_EQ(v->file("banner")->content, "first banner");
    EXPECT_EQ(v->get("MODE")->as_string(), "b");

    // A bad overlay keeps the previous values.
    d.write("app/config/platform.yaml", "Catalog:\n  PageSize: 0\n");
    EXPECT_FALSE(values.refresh());
    EXPECT_EQ(values.current()->get("PAGE_SIZE")->as_int(), 40);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("catalog changed, but the new version was not loaded"), std::string::npos)
        << warnings[0];
    EXPECT_NE(warnings[0].find("PAGE_SIZE: "), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("(out_of_range)"), std::string::npos) << warnings[0];
}

TEST(WatchContract, FirstLoadFailureThrows) {
    TempDir d;
    d.write("etc/app/motd/motd.txt", "NOT LOWER CASE");
    auto c = docuconf::Contract::from_json(std::string(kContract));
    EXPECT_THROW(c.watch(root_env(d)), docuconf::ValidationError);
}

}  // namespace
