// The keySet type (SPEC §4.3), deprecated inputs (SPEC §4.2) and the strict
// parsing rules (SPEC §5), in the declaration API and contract-first mode.
// Needs no file inputs, so it runs in the environment-only build too.
#include <gtest/gtest.h>

#include <sstream>

#include "docuconf/docuconf.hpp"

using docuconf::Code;
using nlohmann::json;

namespace {

const std::string kOld = "old-webhook-key-0123456789abcdef0123";
const std::string kNew = "new-webhook-key-0123456789abcdef0123";

struct Webhooks {
    CLI::App app;
    docuconf::Declaration config{app, "webhooks"};
    docuconf::KeySet keys;
    std::optional<docuconf::KeySet> api_keys;
    std::vector<std::string> warnings;
    Webhooks() {
        config.on_warning([this](const std::string& w) { warnings.push_back(w); });
        config.add_var("WEBHOOK_KEYS", keys, "Keys that verify webhook signatures").key_length(32, 256);
        config.add_var("API_KEYS", api_keys, "Keys that callers present").max_keys(3).delimiter(";");
    }
};

docuconf::ValidationError fails(Webhooks& w, const docuconf::Env& env) {
    try {
        w.config.load(env);
    } catch (const docuconf::ValidationError& e) {
        return e;
    }
    ADD_FAILURE() << "load succeeded, expected violations";
    return docuconf::ValidationError({});
}

TEST(KeySet, BindsTheKeysInOrder) {
    Webhooks w;
    w.config.load({{"WEBHOOK_KEYS", kOld + "," + kNew}});
    EXPECT_EQ(w.keys.keys(), (std::vector<std::string>{kOld, kNew}));
    EXPECT_FALSE(w.api_keys.has_value());
    w.config.load({{"WEBHOOK_KEYS", kOld}, {"API_KEYS", "a;b;c"}});
    EXPECT_EQ(w.keys.size(), 1u);
    EXPECT_EQ(w.api_keys->keys(), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(KeySet, ContainsAndVerify) {
    docuconf::KeySet keys({kOld, kNew});
    EXPECT_TRUE(keys.contains(kOld));
    EXPECT_TRUE(keys.contains(kNew));
    EXPECT_FALSE(keys.contains(kOld.substr(1)));
    EXPECT_FALSE(keys.contains(""));
    int calls = 0;
    // Every key is tried, even after one matches.
    EXPECT_TRUE(keys.verify([&](std::string_view k) {
        ++calls;
        return k == kOld;
    }));
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(keys.verify([](std::string_view) { return false; }));
    EXPECT_FALSE(docuconf::KeySet().contains(kOld));
}

TEST(KeySet, NeverPrintsItsKeys) {
    docuconf::KeySet keys({kOld});
    std::ostringstream os;
    os << keys;
    EXPECT_EQ(os.str(), "***");
    EXPECT_EQ(to_string(keys), "***");
    EXPECT_EQ(json(keys).dump(), "\"***\"");
}

TEST(KeySet, Violations) {
    Webhooks w;
    auto e = fails(w, {{"WEBHOOK_KEYS", kOld + ","}});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::OutOfRange});
    e = fails(w, {{"WEBHOOK_KEYS", kOld + "," + kNew + "," + kNew}});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::TooManyItems});
    e = fails(w, {{"WEBHOOK_KEYS", kOld + ",short-key"}});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(std::string(e.what()).find("short-key"), std::string::npos) << e.what();
    // Keys are never trimmed: the space makes the key one character longer.
    e = fails(w, {{"WEBHOOK_KEYS", kOld + ", " + std::string(256, 'k')}});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::OutOfRange});
    e = fails(w, {{"WEBHOOK_KEYS", "vault:secret/data/webhooks#keys"}});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::InvalidType});
    EXPECT_EQ(std::string(e.what()).find("secret/data"), std::string::npos) << e.what();
    e = fails(w, {});
    EXPECT_EQ(e.codes_for("WEBHOOK_KEYS"), std::vector<Code>{Code::MissingRequired});
}

// SPEC §4.3: an empty key is "key N is empty", N 1-based as received, in
// both modes, and the message never holds a key.
TEST(KeySet, EmptyKeyMessage) {
    struct Case {
        std::string value;
        std::string message;
    };
    const std::vector<Case> cases = {
        {"old,", "key 2 is empty"}, {",new", "key 1 is empty"}, {"a,,b", "key 2 is empty"}};
    for (const auto& c : cases) {
        CLI::App app;
        docuconf::Declaration d{app, "svc"};
        docuconf::KeySet keys;
        d.add_var("KEYS", keys, "Keys that verify signatures").max_keys(3);
        try {
            d.load({{"KEYS", c.value}});
            ADD_FAILURE() << c.value << ": load succeeded";
        } catch (const docuconf::ValidationError& e) {
            ASSERT_EQ(e.violations().size(), 1u) << c.value;
            EXPECT_EQ(e.violations()[0].code, Code::OutOfRange) << c.value;
            EXPECT_EQ(e.violations()[0].message, c.message) << c.value;
        }
    }
    auto contract = docuconf::Contract::from_json(std::string(R"({
        "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "svc"},
        "vars": {"KEYS": {"type": "keySet", "description": "Keys that verify signatures", "secret": true,
                          "maxKeys": 3}}})"));
    for (const auto& c : cases) {
        try {
            contract.load({{"KEYS", c.value}});
            ADD_FAILURE() << c.value << ": load succeeded";
        } catch (const docuconf::ValidationError& e) {
            ASSERT_EQ(e.violations().size(), 1u) << c.value;
            EXPECT_EQ(e.violations()[0].code, Code::OutOfRange) << c.value;
            EXPECT_EQ(e.violations()[0].message, c.message) << c.value;
        }
    }
    // Lengths name keys by the same 1-based position.
    Webhooks w;
    auto e = fails(w, {{"WEBHOOK_KEYS", kOld + ",short-key"}});
    EXPECT_EQ(e.violations()[0].message, "key 2 is 9 characters, below keyMinLength 32");
}

TEST(KeySet, Exports) {
    Webhooks w;
    json c = json::parse(w.config.export_json().dump());
    json k = c["vars"]["WEBHOOK_KEYS"];
    EXPECT_EQ(k["type"], "keySet");
    EXPECT_EQ(k["secret"], true);
    EXPECT_EQ(k["required"], true);
    EXPECT_EQ(k["minKeys"], 1);
    EXPECT_EQ(k["maxKeys"], 2);
    EXPECT_EQ(k["keyMinLength"], 32);
    EXPECT_EQ(k["keyMaxLength"], 256);
    EXPECT_EQ(k["encoding"], "csv");
    EXPECT_EQ(c["vars"]["API_KEYS"]["maxKeys"], 3);
    EXPECT_EQ(c["vars"]["API_KEYS"]["separator"], ";");
    EXPECT_NE(w.config.export_cue().find("type: \"keySet\""), std::string::npos) << w.config.export_cue();
}

TEST(KeySet, DeclarationErrors) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    docuconf::KeySet a, b, c;
    d.add_var("NOT_SECRET", a, "A key set marked not secret").secret(false);
    d.add_var("NO_KEYS", b, "A key set that allows no keys").min_keys(0);
    d.add_var("FEWER_MAX", c, "maxKeys below minKeys").min_keys(3).max_keys(2).key_length(10, 5);
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("NOT_SECRET: a keySet is always secret"), std::string::npos) << what;
        EXPECT_NE(what.find("NO_KEYS: minKeys must be at least 1"), std::string::npos) << what;
        EXPECT_NE(what.find("FEWER_MAX: maxKeys must be at least minKeys"), std::string::npos) << what;
        EXPECT_NE(what.find("FEWER_MAX: keyMinLength is above keyMaxLength"), std::string::npos) << what;
    }
}

TEST(KeySet, ContractFirst) {
    auto contract = docuconf::Contract::from_json(std::string(R"({
        "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "svc"},
        "vars": {"KEYS": {"type": "keySet", "description": "Signing keys, one per variable", "secret": true,
                          "encoding": "indexed", "keyMinLength": 4}}})"));
    auto values = contract.load({{"KEYS__0", "key-old"}, {"KEYS__1", "key-new"}});
    ASSERT_TRUE(values.key_set("KEYS"));
    EXPECT_TRUE(values.key_set("KEYS")->contains("key-new"));
    EXPECT_EQ(values.to_json()["KEYS"], json::parse(R"(["key-old","key-new"])"));
    EXPECT_EQ(values.to_redacted_json()["KEYS"], "***");
    std::ostringstream os;
    os << values;
    EXPECT_EQ(os.str().find("key-old"), std::string::npos);
    try {
        contract.load({{"KEYS__0", "key"}});
        FAIL() << "expected out_of_range";
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("KEYS"), std::vector<Code>{Code::OutOfRange});
    }
    EXPECT_THROW(docuconf::Contract::from_json(std::string(R"({
        "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "svc"},
        "vars": {"KEYS": {"type": "keySet", "description": "A key set that is not secret"}}})")),
                 docuconf::DeclarationError);
}

// ---- deprecated ----

TEST(Deprecated, DeclarationRules) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::optional<int> blank, long_message;
    int required = 0;
    d.add_var("BLANK", blank, "Deprecated with a blank message").deprecated("  ");
    d.add_var("LONG_MESSAGE", long_message, "Deprecated with a long message").deprecated(std::string(501, 'x'));
    d.add_var("REQUIRED", required, "Required and deprecated").deprecated("Use PORT instead", "PORT");
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("BLANK: the deprecated message must not be blank"), std::string::npos) << what;
        EXPECT_NE(what.find("LONG_MESSAGE: the deprecated message is 501 characters; it may have at most 500"),
                  std::string::npos)
            << what;
        EXPECT_NE(what.find("REQUIRED: a required variable cannot be deprecated"), std::string::npos) << what;
    }
    CLI::App app2;
    docuconf::Declaration ok{app2, "svc"};
    std::optional<int> at_limit;
    ok.add_var("AT_LIMIT", at_limit, "Deprecated with 500 characters").deprecated(std::string(500, 'x'));
    EXPECT_NO_THROW(ok.check());
}

TEST(Deprecated, WarnsWithTheMessageNeverTheValue) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    std::vector<std::string> warnings;
    d.on_warning([&](const std::string& w) { warnings.push_back(w); });
    std::optional<std::string> token;
    std::optional<int> old_port;
    d.add_var("OLD_TOKEN", token, "Token of the retired billing API")
        .secret()
        .deprecated("The billing API no longer takes a token");
    d.add_var("OLD_PORT", old_port, "Old name of the listen port").deprecated("Use PORT instead", "PORT");
    d.load({});
    EXPECT_TRUE(warnings.empty());
    d.load({{"OLD_TOKEN", "tok-0123456789"}, {"OLD_PORT", "9090"}});
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0], "OLD_TOKEN is deprecated: The billing API no longer takes a token");
    EXPECT_EQ(warnings[1], "OLD_PORT is deprecated: Use PORT instead; use PORT");
    EXPECT_EQ(*token, "tok-0123456789");
    EXPECT_EQ(*old_port, 9090);
}

TEST(Deprecated, ContractFirstRules) {
    auto contract = [](const std::string& var) {
        return docuconf::Contract::from_json(
            R"({"apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "svc"},
                "vars": {"OLD": )" +
            var + "}}");
    };
    EXPECT_THROW(contract(R"({"type": "int", "description": "Old port", "required": true,
                              "deprecated": {"message": "Use PORT"}})"),
                 docuconf::DeclarationError);
    EXPECT_THROW(contract(R"({"type": "int", "description": "Old port", "deprecated": {"message": ""}})"),
                 docuconf::DeclarationError);
    EXPECT_THROW(contract(R"({"type": "int", "description": "Old port", "deprecated": "Use PORT"})"),
                 docuconf::DeclarationError);
    std::vector<std::string> warnings;
    auto c = contract(R"({"type": "string", "description": "Old token", "secret": true,
                          "deprecated": {"message": "Going away"}})");
    c.on_warning([&](const std::string& w) { warnings.push_back(w); });
    c.load({{"OLD", "tok-0123456789"}});
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "OLD is deprecated: Going away");
}

// ---- strict parsing (SPEC §5) ----

struct Strict {
    CLI::App app;
    docuconf::Declaration config{app, "strict"};
    std::optional<bool> flag;
    std::optional<std::int64_t> count;
    std::optional<double> ratio;
    std::optional<std::chrono::nanoseconds> wait;
    std::optional<std::vector<std::string>> names;
    Strict() {
        config.add_var("FLAG", flag, "A switch with no default");
        config.add_var("COUNT", count, "A signed integer");
        config.add_var("RATIO", ratio, "A float with no bounds");
        config.add_var("WAIT", wait, "A Go duration");
        config.add_var("NAMES", names, "Names in csv");
    }
};

std::vector<Code> strict_codes(const std::string& name, const std::string& value) {
    Strict s;
    try {
        s.config.load({{name, value}});
        return {};
    } catch (const docuconf::ValidationError& e) {
        return e.codes_for(name);
    }
}

TEST(StrictParsing, AcceptsExactlyTheSpecForms) {
    Strict s;
    s.config.load({{"FLAG", "tRuE"}, {"COUNT", "+007"}, {"RATIO", "25e-2"}, {"WAIT", "-1m30s"}, {"NAMES", "a, b ,"}});
    EXPECT_TRUE(*s.flag);
    EXPECT_EQ(*s.count, 7);
    EXPECT_DOUBLE_EQ(*s.ratio, 0.25);
    EXPECT_EQ(*s.wait, -std::chrono::seconds(90));
    EXPECT_EQ(*s.names, (std::vector<std::string>{"a", " b ", ""}));
    s.config.load({{"COUNT", "-0"}, {"RATIO", "+1.5"}, {"WAIT", "0"}});
    EXPECT_EQ(*s.count, 0);
    EXPECT_DOUBLE_EQ(*s.ratio, 1.5);
}

TEST(StrictParsing, RejectsWhatLenientHostsAccept) {
    const std::vector<Code> invalid{Code::InvalidType};
    for (const char* v : {"1", "0", "t", "f", "yes", "no", "on", "off", " true", "false ", "true\n"})
        EXPECT_EQ(strict_codes("FLAG", v), invalid) << v;
    for (const char* v : {"0x10", "0o17", "0b101", "1_000", "1e3", "5.0", " 5", "5\n", "-", "+-5", "\xd9\xa1\xd9\xa2"})
        EXPECT_EQ(strict_codes("COUNT", v), invalid) << v;
    EXPECT_EQ(strict_codes("COUNT", "-9223372036854775809"), std::vector<Code>{Code::OutOfRange});
    EXPECT_EQ(strict_codes("COUNT", "+9223372036854775808"), std::vector<Code>{Code::OutOfRange});
    for (const char* v : {"0x1p4", "inf", "Infinity", "-inf", "nan", ".5", "5.", "1_000.5", " 1.5", "1.5\n", "1e",
                          "1e400", "0,5"})
        EXPECT_EQ(strict_codes("RATIO", v), invalid) << v;
    for (const char* v : {"5", "5S", "1d", "1m 30s", "5s\n"}) EXPECT_EQ(strict_codes("WAIT", v), invalid) << v;
}

}  // namespace
