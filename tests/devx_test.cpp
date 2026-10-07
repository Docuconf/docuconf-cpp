// First-use ergonomics: schemas written from member types, chrono durations
// everywhere, contract literals, and secrets redacted when printed.
#include <gtest/gtest.h>

#include <map>
#include <sstream>

#include "docuconf/docuconf.hpp"

namespace app {

struct Route {
    std::string prefix;
    std::string upstream;
    std::uint16_t weight = 1;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Route, prefix, upstream, weight)
DOCUCONF_DEFINE_SCHEMA(Route, prefix, upstream, weight)

// Schema only: optional members are not required.
struct Limits {
    int per_minute = 0;
    std::optional<std::int64_t> burst;
};
DOCUCONF_DEFINE_SCHEMA(Limits, per_minute, burst)

struct Routes {
    std::vector<Route> routes;
    std::map<std::string, double> weights;
    bool strict = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Routes, routes, weights, strict)
DOCUCONF_DEFINE_SCHEMA(Routes, routes, weights, strict)

}  // namespace app

namespace {

TEST(DefineSchema, WritesTheSchemaFromMemberTypes) {
    auto s = docuconf::json_schema<app::Routes>::get();
    EXPECT_EQ(s["type"], "object");
    EXPECT_EQ(s["required"], (nlohmann::json{"routes", "weights", "strict"}));
    EXPECT_EQ(s["properties"]["strict"], (nlohmann::json{{"type", "boolean"}}));
    EXPECT_EQ(s["properties"]["weights"]["additionalProperties"], (nlohmann::json{{"type", "number"}}));
    auto route = s["properties"]["routes"]["items"];
    EXPECT_EQ(route["required"], (nlohmann::json{"prefix", "upstream", "weight"}));
    EXPECT_EQ(route["properties"]["weight"],
              (nlohmann::json{{"type", "integer"}, {"minimum", 0}, {"maximum", 65535}}));
}

TEST(DefineSchema, OptionalMembersAreNotRequired) {
    auto s = docuconf::json_schema<app::Limits>::get();
    EXPECT_EQ(s["required"], (nlohmann::json{"per_minute"}));
    EXPECT_EQ(s["properties"]["burst"], (nlohmann::json{{"type", "integer"}}));
}

TEST(DefineSchema, ChecksAJsonVariable) {
    CLI::App cli;
    docuconf::Declaration d{cli, "svc"};
    app::Routes routes;
    d.add_var("ROUTES", routes, "Routing table as JSON");
    d.load({{"ROUTES", R"({"routes":[{"prefix":"/a","upstream":"http://a","weight":3}],"weights":{},"strict":true})"}});
    ASSERT_EQ(routes.routes.size(), 1u);
    EXPECT_EQ(routes.routes[0].weight, 3);
    EXPECT_TRUE(routes.strict);
    try {
        d.load({{"ROUTES", R"({"routes":[{"prefix":"/a","upstream":"u","weight":70000}],"weights":{},"strict":true})"}});
        FAIL();
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("ROUTES"), std::vector<docuconf::Code>{docuconf::Code::SchemaMismatch}) << e.what();
    }
}

TEST(Contract, FromAStringLiteral) {
    auto c = docuconf::Contract::from_json(R"({
        "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract",
        "metadata": {"name": "svc", "generator": {"name": "docuconf-cpp", "language": "cpp", "version": "0.1.0"}},
        "vars": {"PORT": {"type": "int", "description": "HTTP listen port", "default": 8080},
                 "TOKEN": {"type": "string", "description": "API token", "secret": true, "required": true}}
    })");
    auto values = c.load({{"TOKEN", "hunter2-token"}});
    EXPECT_EQ(values.get("PORT")->as_int(), 8080);
    std::ostringstream os;
    os << values;
    EXPECT_EQ(os.str().find("hunter2"), std::string::npos) << os.str();
    EXPECT_NE(os.str().find(R"("TOKEN":"***")"), std::string::npos) << os.str();
}

#if DOCUCONF_FILE_INPUTS
TEST(Files, MinRemainingTakesAChronoDuration) {
    using namespace std::chrono_literals;
    CLI::App cli;
    docuconf::Declaration d{cli, "svc"};
    docuconf::TlsKeyPair tls;
    d.add_file("tls", tls, "Serving certificate").path("/etc/svc/tls").min_remaining(720h);
    d.check();
    EXPECT_EQ(*d.file_specs()[0].min_remaining, std::chrono::hours(720));
    CLI::App cli2;
    docuconf::Declaration d2{cli2, "svc"};
    d2.add_file("tls", tls, "Serving certificate").path("/etc/svc/tls").min_remaining("720h");
    d2.check();
    EXPECT_EQ(*d2.file_specs()[0].min_remaining, std::chrono::hours(720));
}
#endif

}  // namespace
