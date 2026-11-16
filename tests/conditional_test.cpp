// Contract-first mode: conditional requirements (requiredIf, SPEC §4.2.1),
// roles (SPEC §4.9) and well-known fragments (includes, SPEC §4.8). The
// shared suite's pending cases cover the codes; these pin the messages, role
// narrowing and the reader's refusals.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

#include "docuconf/contract.hpp"

using docuconf::Code;
using nlohmann::json;

namespace {

json base(json vars) {
    return json{{"apiVersion", "docuconf.dev/v1alpha1"},
                {"kind", "ConfigContract"},
                {"metadata", {{"name", "svc"}}},
                {"vars", std::move(vars)}};
}

const json kConditional = base(json::parse(R"({
  "TLS_ENABLED": {"type": "bool", "description": "Serve over TLS", "default": false},
  "LOG_FORMAT": {"type": "enum", "description": "Log line format", "values": ["text", "json"], "default": "text"},
  "WORKERS": {"type": "int", "description": "Worker threads", "default": 1},
  "CA_SUBJECT": {"type": "string", "description": "Client CA subject", "requiredIf": {"TLS_ENABLED": true}},
  "LOG_SCHEMA_URL": {"type": "url", "description": "Log schema location", "requiredIf": {"LOG_FORMAT": "json"}},
  "POOL_NAME": {"type": "string", "description": "Name of the worker pool", "requiredIf": {"WORKERS": 4}}
})"));

std::vector<docuconf::Violation> failures(const docuconf::Contract& c, const docuconf::Env& env) {
    try {
        c.load(env);
    } catch (const docuconf::ValidationError& e) {
        return e.violations();
    }
    return {};
}

std::string messages(const json& contract) {
    try {
        docuconf::Contract::from_json(contract);
    } catch (const docuconf::DeclarationError& e) {
        std::string out;
        for (const auto& p : e.problems()) out += p + "\n";
        return out;
    }
    return "";
}

TEST(RequiredIf, OffByDefault) {
    auto c = docuconf::Contract::from_json(kConditional);
    auto v = c.load({});
    EXPECT_EQ(v.get("CA_SUBJECT"), nullptr);
    EXPECT_EQ(v.get("LOG_SCHEMA_URL"), nullptr);
    ASSERT_NE(c.required_if("CA_SUBJECT"), nullptr);
    EXPECT_EQ(c.required_if("CA_SUBJECT")->str(), "TLS_ENABLED is true");
    EXPECT_EQ(c.required_if("TLS_ENABLED"), nullptr);
}

TEST(RequiredIf, MessagesWriteTheValueAsJson) {
    auto c = docuconf::Contract::from_json(kConditional);
    auto v = failures(c, {{"TLS_ENABLED", "TRUE"}, {"LOG_FORMAT", "json"}, {"WORKERS", "4"}});
    ASSERT_EQ(v.size(), 3u);
    std::map<std::string, std::string> got;
    for (const auto& x : v) {
        EXPECT_EQ(x.code, Code::MissingRequired);
        got[x.input] = x.message;
    }
    EXPECT_EQ(got["CA_SUBJECT"], "is required when TLS_ENABLED is true");
    EXPECT_EQ(got["LOG_SCHEMA_URL"], "is required when LOG_FORMAT is \"json\"");
    EXPECT_EQ(got["POOL_NAME"], "is required when WORKERS is 4");
}

TEST(RequiredIf, ConditionsCompareTheLoadedValue) {
    json contract = kConditional;
    contract["vars"]["TLS_ENABLED"]["default"] = true;
    auto c = docuconf::Contract::from_json(contract);
    // The default turns the condition on, and the environment turns it off.
    EXPECT_EQ(failures(c, {}).size(), 1u);
    EXPECT_TRUE(failures(c, {{"TLS_ENABLED", "false"}}).empty());
    EXPECT_EQ(c.load({{"CA_SUBJECT", "CN=ca"}}).get("CA_SUBJECT")->as_string(), "CN=ca");
}

TEST(RequiredIf, AProfileTurnsAConditionOn) {
    json contract = kConditional;
    contract["vars"]["APP_ENV"] = {{"type", "string"}, {"description", "Which profile loads"}, {"default", "dev"}};
    contract["profiles"] = {{"selector", "APP_ENV"}, {"default", "dev"},
                            {"defaults", {{"prod", {{"TLS_ENABLED", true}}}}}};
    auto c = docuconf::Contract::from_json(contract);
    EXPECT_TRUE(failures(c, {}).empty());
    auto v = failures(c, {{"APP_ENV", "prod"}});
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].input, "CA_SUBJECT");
}

TEST(RequiredIf, AnInvalidConditionVariableMakesNothingRequired) {
    auto c = docuconf::Contract::from_json(kConditional);
    auto v = failures(c, {{"TLS_ENABLED", "yes"}});
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].input, "TLS_ENABLED");
    EXPECT_EQ(v[0].code, Code::InvalidType);
}

TEST(RequiredIf, ABoolIsNotAnInteger) {
    // 1 is not true: the condition holds only for an int variable's 1.
    json contract = kConditional;
    contract["vars"]["CA_SUBJECT"]["requiredIf"] = {{"WORKERS", 1}};
    auto c = docuconf::Contract::from_json(contract);
    EXPECT_EQ(failures(c, {}).size(), 1u);
    contract["vars"]["CA_SUBJECT"]["requiredIf"] = {{"TLS_ENABLED", 1}};
    EXPECT_NE(messages(contract).find("CA_SUBJECT: requiredIf: TLS_ENABLED is 1 is not a value TLS_ENABLED can have"),
              std::string::npos)
        << messages(contract);
}

TEST(RequiredIf, TheReaderRefusesBadConditions) {
    auto refused = [](const std::string& var, const json& spec, const std::string& want) {
        json contract = kConditional;
        contract["vars"]["SECRET_MODE"] = {{"type", "string"}, {"description", "A secret mode"}, {"secret", true}};
        contract["vars"]["TIMEOUT"] = {{"type", "duration"}, {"description", "Request timeout"}, {"default", "1m"}};
        contract["vars"][var] = spec;
        std::string got = messages(contract);
        EXPECT_NE(got.find(want), std::string::npos) << "want " << want << "\ngot " << got;
    };
    json s = {{"type", "string"}, {"description", "Conditional input"}};
    auto with = [&](json extra) {
        json out = s;
        out.update(extra);
        return out;
    };
    refused("X", with({{"requiredIf", json::object()}}), "X: requiredIf must name one variable and its value");
    refused("X", with({{"requiredIf", {{"TLS_ENABLED", true}, {"WORKERS", 2}}}}),
            "X: requiredIf must name one variable and its value");
    refused("X", with({{"requiredIf", {{"WORKERS", 1.5}}}}),
            "X: requiredIf must name one variable and a string, integer or boolean value");
    refused("X", with({{"requiredIf", {{"WORKERS", nullptr}}}}), "X: requiredIf must name one variable");
    refused("X", with({{"requiredIf", {{"X", "on"}}}}), "X: requiredIf must name another declared variable, not \"X\"");
    refused("X", with({{"requiredIf", {{"NOPE", "on"}}}}), "X: requiredIf must name another declared variable");
    refused("X", with({{"requiredIf", {{"SECRET_MODE", "on"}}}}),
            "X: requiredIf names SECRET_MODE, which is not a plain string, int, bool or enum variable");
    refused("X", with({{"requiredIf", {{"TIMEOUT", "1m"}}}}), "X: requiredIf names TIMEOUT, which is not a plain");
    refused("X", with({{"requiredIf", {{"LOG_FORMAT", "xml"}}}}),
            "X: requiredIf: LOG_FORMAT is \"xml\" is not a value LOG_FORMAT can have");
    refused("X", with({{"requiredIf", {{"WORKERS", "4"}}}}), "X: requiredIf: WORKERS is \"4\" is not a value");
    refused("X", with({{"requiredIf", {{"TLS_ENABLED", true}}}, {"required", true}}),
            "X: an input with requiredIf cannot be required or have a default");
    refused("X", with({{"requiredIf", {{"TLS_ENABLED", true}}}, {"default", "x"}}),
            "X: an input with requiredIf cannot be required or have a default");
    refused("X", with({{"requiredIf", {{"TLS_ENABLED", true}}}, {"deprecated", {{"message", "Going away soon"}}}}),
            "X: a conditionally required input cannot be deprecated");
}

const json kRoles = [] {
    json c = base(json::parse(R"({
      "DOCUCONF_ROLE": {"type": "enum", "description": "The role this workload runs", "required": true,
                        "values": ["web", "worker"]},
      "ALLOWED_ORIGINS": {"type": "list", "items": "string", "description": "Origins the web server accepts",
                          "required": true, "roles": ["web"]},
      "QUEUES": {"type": "list", "items": "string", "description": "Queues the worker drains", "required": true,
                 "roles": ["worker"]},
      "DATABASE_URL": {"type": "url", "description": "Primary database", "required": true}
    })"));
    c["roles"] = {"web", "worker"};
    return c;
}();

std::set<std::string> failing(const docuconf::Contract& c, const docuconf::Env& env) {
    std::set<std::string> out;
    for (const auto& v : failures(c, env)) out.insert(v.input + "/" + docuconf::to_string(v.code));
    return out;
}

TEST(Roles, AWorkloadRequiresItsRolesInputsOnly) {
    auto c = docuconf::Contract::from_json(kRoles);
    EXPECT_EQ(c.roles(), (std::vector<std::string>{"web", "worker"}));
    EXPECT_EQ(c.input_roles("QUEUES"), (std::vector<std::string>{"worker"}));
    EXPECT_TRUE(c.input_roles("DATABASE_URL").empty());
    EXPECT_EQ(failing(c, {{"DOCUCONF_ROLE", "worker"}}),
              (std::set<std::string>{"QUEUES/missing_required", "DATABASE_URL/missing_required"}));
    EXPECT_EQ(failing(c, {{"DOCUCONF_ROLE", "web"}, {"DATABASE_URL", "postgres://db"}}),
              (std::set<std::string>{"ALLOWED_ORIGINS/missing_required"}));
    auto v = c.load({{"DOCUCONF_ROLE", "worker"}, {"DATABASE_URL", "postgres://db"}, {"QUEUES", "a,b"}});
    EXPECT_EQ(v.get("ALLOWED_ORIGINS"), nullptr);
    EXPECT_EQ(v.get("DOCUCONF_ROLE")->as_string(), "worker");
}

TEST(Roles, WithoutAValidRoleOnlyTheRoleIsReported) {
    auto c = docuconf::Contract::from_json(kRoles);
    EXPECT_EQ(failing(c, {{"DATABASE_URL", "postgres://db"}}), (std::set<std::string>{"DOCUCONF_ROLE/missing_required"}));
    EXPECT_EQ(failing(c, {{"DOCUCONF_ROLE", "cron"}, {"DATABASE_URL", "postgres://db"}}),
              (std::set<std::string>{"DOCUCONF_ROLE/not_in_enum"}));
}

TEST(Roles, AConditionOutsideTheRoleRequiresNothing) {
    json contract = kRoles;
    contract["vars"]["TLS_ENABLED"] = {{"type", "bool"}, {"description", "Serve over TLS"}, {"default", true}};
    contract["vars"]["CA_SUBJECT"] = {{"type", "string"}, {"description", "Client CA subject"},
                                      {"requiredIf", {{"TLS_ENABLED", true}}}, {"roles", {"web"}}};
    auto c = docuconf::Contract::from_json(contract);
    docuconf::Env env = {{"DATABASE_URL", "postgres://db"}, {"QUEUES", "a"}, {"ALLOWED_ORIGINS", "x"}};
    env["DOCUCONF_ROLE"] = "worker";
    EXPECT_TRUE(failing(c, env).empty());
    env["DOCUCONF_ROLE"] = "web";
    EXPECT_EQ(failing(c, env), (std::set<std::string>{"CA_SUBJECT/missing_required"}));
}

TEST(Roles, TheReaderRefusesBadRoles) {
    auto refused = [](const json& contract, const std::string& want) {
        std::string got = messages(contract);
        EXPECT_NE(got.find(want), std::string::npos) << "want " << want << "\ngot " << got;
    };
    json c = kRoles;
    c["roles"] = json::array();
    refused(c, "roles: roles must be a list of at least one role name");
    c["roles"] = {"Web", "worker"};
    refused(c, "roles: role \"Web\" must be a DNS label");
    c["roles"] = {"web", "worker", "web"};
    refused(c, "roles: role web is listed twice");
    c = kRoles;
    c["vars"]["QUEUES"]["roles"] = {"cron"};
    refused(c, "QUEUES: role cron is not one of the contract's roles, web, worker");
    c = kRoles;
    c["vars"]["QUEUES"]["roles"] = json::array();
    refused(c, "QUEUES: roles must be a list of at least one role name");
    c = kRoles;
    c.erase("roles");
    c["vars"].erase("DOCUCONF_ROLE");
    refused(c, "QUEUES: has roles, but the contract declares none");
    c = kRoles;
    c["vars"].erase("DOCUCONF_ROLE");
    refused(c, "DOCUCONF_ROLE: a contract with roles declares it as a required enum whose values are the roles, web, "
               "worker, used by every role");
    c = kRoles;
    c["vars"]["DOCUCONF_ROLE"]["values"] = {"worker", "web"};
    refused(c, "DOCUCONF_ROLE: a contract with roles declares it");
    c = kRoles;
    c["vars"]["DOCUCONF_ROLE"]["required"] = false;
    refused(c, "DOCUCONF_ROLE: a contract with roles declares it");
    c = kRoles;
    c["vars"]["DOCUCONF_ROLE"]["roles"] = {"web"};
    refused(c, "DOCUCONF_ROLE: a contract with roles declares it");
}

TEST(Includes, AreReadAndExposed) {
    json c = base(json::parse(R"({"GOMAXPROCS": {"type": "int", "description": "Maximum OS threads", "min": 1}})"));
    c["includes"] = {"go-runtime@1", "otel@1"};
    EXPECT_EQ(docuconf::Contract::from_json(c).includes(), (std::vector<std::string>{"go-runtime@1", "otel@1"}));
    c["includes"] = {"go-runtime@1", "go-runtime@1"};
    EXPECT_NE(messages(c).find("includes: go-runtime@1 is listed twice"), std::string::npos);
    c["includes"] = {"go-runtime"};
    EXPECT_NE(messages(c).find("includes: \"go-runtime\" is not a fragment name, name@major"), std::string::npos);
    c["includes"] = {"otel@0"};
    EXPECT_NE(messages(c).find("is not a fragment name"), std::string::npos);
    c["includes"] = "otel@1";
    EXPECT_NE(messages(c).find("includes must be a list of fragment names"), std::string::npos);
}

#if DOCUCONF_FILE_INPUTS
TEST(RequiredIf, AMissingFileNamesTheCondition) {
    std::random_device rd;
    auto root = std::filesystem::temp_directory_path() / ("docuconf-conditional-" + std::to_string(rd()));
    std::filesystem::create_directories(root);
    json contract = kConditional;
    contract["files"] = {{"allowlist",
                          {{"type", "text"},
                           {"description", "Allowed client names"},
                           {"path", "/etc/app/allowlist/allowlist.txt"},
                           {"requiredIf", {{"TLS_ENABLED", true}}}}}};
    auto c = docuconf::Contract::from_json(contract);
    docuconf::Env env = {{"DOCUCONF_FILE_ROOT", root.string()}, {"CA_SUBJECT", "CN=ca"}};
    EXPECT_TRUE(failures(c, env).empty());
    env["TLS_ENABLED"] = "true";
    auto v = failures(c, env);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].input, "allowlist");
    EXPECT_EQ(v[0].code, Code::FileMissing);
    EXPECT_EQ(v[0].message,
              (root / "etc/app/allowlist/allowlist.txt").string() + " does not exist, and is required when TLS_ENABLED is true");
    std::filesystem::create_directories(root / "etc/app/allowlist");
    std::ofstream(root / "etc/app/allowlist/allowlist.txt") << "client-a\n";
    EXPECT_EQ(c.load(env).file("allowlist")->content, "client-a\n");
    std::filesystem::remove_all(root);
}
#endif

}  // namespace
