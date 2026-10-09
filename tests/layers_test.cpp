// Contract-first mode beyond variables: profiles (SPEC §4.4), config-file
// overlays (SPEC §4.7) and file inputs (SPEC §4.6), read from under
// DOCUCONF_FILE_ROOT. The shared suite covers most cases; these pin what
// it cannot see, such as warnings.
#include <gtest/gtest.h>

#include "test_util.hpp"

using docuconf::Code;

namespace {

const char* kContract = R"({
  "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "catalog"},
  "vars": {
    "APP_ENV": {"type": "string", "description": "Which profile file loads", "default": "Production"},
    "PAGE_SIZE": {"type": "int", "description": "Items per page", "configKey": "Catalog.PageSize",
                  "min": 1, "default": 10},
    "API_TOKEN": {"type": "string", "description": "Token for the search API", "secret": true,
                  "configKey": "Search.Token"},
    "TAGS": {"type": "list", "description": "Featured tags", "items": "string", "encoding": "csv",
             "separator": ",", "configKey": "Catalog.Tags"},
    "LIMITS": {"type": "json", "description": "Rate limits as JSON", "configKey": "Catalog.Limits"}
  },
  "profiles": {"selector": "APP_ENV", "default": "Production",
               "defaults": {"Production": {"PAGE_SIZE": 20}, "": {"PAGE_SIZE": 30}}},
  "overlays": {"platform": {"format": "yaml", "path": "/app/config/platform.yaml", "keySeparator": "."}},
  "files": {"licence": {"type": "text", "description": "Licence key", "path": "/etc/app/licence/licence.key",
                        "deprecated": {"message": "Licences are no longer checked"}}}
})";

struct Loaded {
    std::vector<std::string> warnings;
    docuconf::Values values;
};

Loaded load(const testutil::TempDir& root, docuconf::Env env) {
    Loaded out;
    auto c = docuconf::Contract::from_json(std::string(kContract));
    c.on_warning([&](const std::string& w) { out.warnings.push_back(w); });
    env["DOCUCONF_FILE_ROOT"] = root.str();
    out.values = c.load(env);
    return out;
}

TEST(Layers, DefaultThenProfileThenOverlayThenEnvironment) {
    testutil::TempDir root;
    EXPECT_EQ(load(root, {{"APP_ENV", "Development"}}).values.get("PAGE_SIZE")->as_int(), 10);
    EXPECT_EQ(load(root, {}).values.get("PAGE_SIZE")->as_int(), 20);
    root.write("app/config/platform.yaml", "Catalog:\n  PageSize: 40\n  Tags: [a, b]\n  Limits: {perMinute: 60}\n");
    auto l = load(root, {});
    EXPECT_EQ(l.values.get("PAGE_SIZE")->as_int(), 40);
    EXPECT_EQ(l.values.to_json()["TAGS"], json::parse(R"(["a","b"])"));
    EXPECT_EQ(l.values.to_json()["LIMITS"], json::parse(R"({"perMinute":60})"));
    EXPECT_TRUE(l.warnings.empty());
    l = load(root, {{"PAGE_SIZE", "50"}});
    EXPECT_EQ(l.values.get("PAGE_SIZE")->as_int(), 50);
    ASSERT_EQ(l.warnings.size(), 1u);
    EXPECT_EQ(l.warnings[0], "PAGE_SIZE is set in the environment and in overlay platform; the environment wins");
}

TEST(Layers, AnEmptyStringSelectorNamesAProfile) {
    testutil::TempDir root;
    EXPECT_EQ(load(root, {{"APP_ENV", ""}}).values.get("PAGE_SIZE")->as_int(), 30);
}

TEST(Layers, ASecretIsNeverReadFromAnOverlay) {
    testutil::TempDir root;
    root.write("app/config/platform.yaml", "Search:\n  Token: tok-0123456789\n");
    try {
        load(root, {});
        FAIL() << "expected invalid_type";
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("API_TOKEN"), std::vector<Code>{Code::InvalidType});
        EXPECT_EQ(std::string(e.what()).find("tok-0123456789"), std::string::npos) << e.what();
    }
}

TEST(Layers, AnOverlayThatDoesNotParseIsReportedForTheOverlay) {
    testutil::TempDir root;
    root.write("app/config/platform.yaml", "Catalog: [\n");
    try {
        load(root, {});
        FAIL() << "expected file_malformed";
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("platform"), std::vector<Code>{Code::FileMalformed});
    }
}

TEST(Layers, ADeprecatedFileThatIsPresentWarns) {
    testutil::TempDir root;
    auto l = load(root, {});
    EXPECT_TRUE(l.warnings.empty());
    EXPECT_EQ(l.values.file("licence"), nullptr);
    root.write("etc/app/licence/licence.key", "ABC-123");
    l = load(root, {});
    ASSERT_NE(l.values.file("licence"), nullptr);
    EXPECT_EQ(l.values.file("licence")->content, "ABC-123");
    ASSERT_EQ(l.warnings.size(), 1u);
    EXPECT_EQ(l.warnings[0], "licence is deprecated: Licences are no longer checked");
}

TEST(Layers, WatchIsRejected) {
    std::string c = kContract;
    const std::string sep = "\"keySeparator\": \".\"";
    c.replace(c.find(sep), sep.size(), sep + ", \"reload\": \"watch\"");
    EXPECT_THROW(docuconf::Contract::from_json(c), docuconf::DeclarationError);
}

}  // namespace
