// The shared conformance suite (SPEC §12), run through contract-first mode
// as docuconf-go's conformance/README.md describes.
//
// cases.json is found through DOCUCONF_CONFORMANCE, falling back to
// ../docuconf-go/conformance/cases.json next to this repository. With
// DOCUCONF_REQUIRE_CONFORMANCE=1 a missing file fails the test instead of
// skipping it.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "docuconf/contract.hpp"

namespace {

using nlohmann::json;

// Capability tags this SDK does not support. Empty: int64 values are held
// as std::int64_t, and json values are checked against their JSON Schema.
const std::set<std::string> kUnsupported = {};

std::string cases_path() {
    if (const char* p = std::getenv("DOCUCONF_CONFORMANCE"); p && *p) return p;
    return std::string(DOCUCONF_SOURCE_DIR) + "/../docuconf-go/conformance/cases.json";
}

bool same(const json& got, const json& want) {
    if (got.is_number() && want.is_number()) {
        bool gi = got.is_number_integer(), wi = want.is_number_integer();
        if (gi && wi) {
            // Integers compare exactly, whether signed or unsigned.
            if (got.is_number_unsigned() || want.is_number_unsigned())
                return got.dump() == want.dump();
            return got.get<std::int64_t>() == want.get<std::int64_t>();
        }
        return got.get<double>() == want.get<double>();
    }
    if (got.is_array() && want.is_array()) {
        if (got.size() != want.size()) return false;
        for (std::size_t i = 0; i < got.size(); ++i)
            if (!same(got[i], want[i])) return false;
        return true;
    }
    if (got.is_object() && want.is_object()) {
        if (got.size() != want.size()) return false;
        for (const auto& [k, v] : want.items())
            if (!got.contains(k) || !same(got[k], v)) return false;
        return true;
    }
    return got == want;
}

// The raw env values of secret variables (including indexed items).
std::vector<std::string> secret_values(const json& c) {
    std::vector<std::string> out;
    for (const auto& [name, spec] : c["contract"]["vars"].items()) {
        if (!spec.value("secret", false)) continue;
        for (const auto& [k, v] : c["env"].items()) {
            if ((k == name || k.rfind(name + "__", 0) == 0) && !v.get<std::string>().empty())
                out.push_back(v.get<std::string>());
        }
    }
    return out;
}

std::string run_case(const json& c) {
    docuconf::Env env;
    for (const auto& [k, v] : c["env"].items()) env[k] = v.get<std::string>();
    docuconf::Contract contract = docuconf::Contract::from_json(c["contract"]);
    if (c.contains("expect")) {
        try {
            json got = contract.load(env).to_json();
            for (const auto& [k, want] : c["expect"].items()) {
                if (!same(got.value(k, json()), want))
                    return k + ": got " + got.value(k, json()).dump() + ", want " + want.dump();
            }
            return "";
        } catch (const docuconf::ValidationError& e) {
            return std::string("unexpected failure: ") + e.what();
        }
    }
    std::set<std::pair<std::string, std::string>> want;
    for (const auto& e : c["errors"]) want.emplace(e["var"].get<std::string>(), e["code"].get<std::string>());
    try {
        contract.load(env);
        return "loaded, but expected errors";
    } catch (const docuconf::ValidationError& e) {
        std::set<std::pair<std::string, std::string>> got;
        for (const auto& v : e.violations()) got.emplace(v.input, docuconf::to_string(v.code));
        std::string out = e.what();
        for (const auto& s : secret_values(c)) {
            if (out.find(s) != std::string::npos) return "error output contains a secret value";
        }
        if (got != want) return std::string("got errors:\n") + e.what();
        return "";
    }
}

TEST(Conformance, Cases) {
    std::string path = cases_path();
    std::ifstream f(path);
    if (!f) {
        const char* req = std::getenv("DOCUCONF_REQUIRE_CONFORMANCE");
        if (req && std::string(req) == "1") FAIL() << "conformance cases not found at " << path;
        GTEST_SKIP() << "conformance cases not found at " << path << " (set DOCUCONF_CONFORMANCE)";
    }
    json doc = json::parse(f);
    int passed = 0, skipped = 0;
    std::vector<std::string> failed;
    std::map<std::string, int> skipped_tags;
    for (const auto& c : doc["cases"]) {
        bool skip = false;
        for (const auto& t : c.value("requires", json::array())) {
            if (kUnsupported.count(t.get<std::string>())) {
                skip = true;
                ++skipped_tags[t.get<std::string>()];
            }
        }
        if (skip) {
            ++skipped;
            continue;
        }
        std::string problem;
        try {
            problem = run_case(c);
        } catch (const std::exception& e) {
            problem = std::string("exception: ") + e.what();
        }
        if (problem.empty()) {
            ++passed;
        } else {
            failed.push_back(c["id"].get<std::string>() + ": " + problem);
        }
    }
    std::ostringstream summary;
    summary << "conformance: " << passed << " passed, " << skipped << " skipped, " << failed.size()
            << " failed, of " << doc["cases"].size() << " cases";
    for (const auto& [t, n] : skipped_tags) summary << " (skipped " << n << " requiring " << t << ")";
    std::cout << summary.str() << std::endl;
    for (const auto& m : failed) ADD_FAILURE() << m;
}

}  // namespace
