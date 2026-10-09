// The shared conformance suite (SPEC §12), run through contract-first mode
// as docuconf-go's conformance/README.md describes.
//
// cases.json is found through DOCUCONF_CONFORMANCE, falling back to
// ../docuconf-go/conformance/cases.json next to this repository. With
// DOCUCONF_REQUIRE_CONFORMANCE=1 a missing file fails the test instead of
// skipping it, and with DOCUCONF_REQUIRE_NO_SKIPS=1 a skipped case fails it
// too (CI sets both).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "docuconf/contract.hpp"

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// The capability tags this SDK supports (conformance/README.md). A case
// that requires any other tag, including one this runner does not know,
// is skipped, never run. int64 values are held as std::int64_t, and json
// values are checked against their JSON Schema. File inputs and overlays
// need the full build (DOCUCONF_FILE_INPUTS=ON): the environment-only
// build has no TLS, YAML or TOML readers.
const std::set<std::string> kSupported = {
    "int64", "json-schema", "key-set", "deprecated", "strict-parsing", "profiles",
#if DOCUCONF_FILE_INPUTS
    "files", "overlays",
#endif
};

bool env_is(const char* name, const char* value) {
    const char* v = std::getenv(name);
    return v && std::string(v) == value;
}

std::string cases_path() {
    if (const char* p = std::getenv("DOCUCONF_CONFORMANCE"); p && *p) return p;
    return std::string(DOCUCONF_SOURCE_DIR) + "/../docuconf-go/conformance/cases.json";
}

bool same(const json& got, const json& want) {
    if (got.is_number() && want.is_number()) {
        bool gi = got.is_number_integer(), wi = want.is_number_integer();
        if (gi && wi) {
            // Integers compare exactly, whether signed or unsigned.
            if (got.is_number_unsigned() || want.is_number_unsigned()) return got.dump() == want.dump();
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

std::string base64_decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto p = chars.find(c);
        if (p == std::string::npos) throw std::runtime_error("invalid base64");
        buf = (buf << 6) | static_cast<unsigned>(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// A new, empty directory for one case, removed afterwards.
struct CaseDir {
    fs::path path;
    CaseDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("docuconf-conformance-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~CaseDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string run_case(const json& c) {
    CaseDir dir;
    docuconf::Env env;
    for (const auto& [k, v] : c["env"].items()) env[k] = v.get<std::string>();
    // Every case gets its own file root, files or not, so no case reads the
    // machine's own files.
    env["DOCUCONF_FILE_ROOT"] = dir.path.string();
    const json files = c.value("files", json::object());
    for (const auto& [p, f] : files.items()) {
        std::string data;
        if (f.contains("text")) data = f["text"].get<std::string>();
        else if (f.contains("base64")) data = base64_decode(f["base64"].get<std::string>());
        else return "file " + p + " has neither text nor base64";
        fs::path full = dir.path / fs::path(p).relative_path();
        fs::create_directories(full.parent_path());
        std::ofstream(full, std::ios::binary) << data;
    }
    std::vector<std::string> warnings;
    docuconf::Contract contract = docuconf::Contract::from_json(c["contract"]);
    contract.on_warning([&](const std::string& w) { warnings.push_back(w); });
    auto leaked = [&](const std::string& out) -> std::string {
        for (const auto& s : secret_values(c)) {
            if (out.find(s) != std::string::npos) return "error output contains a secret value";
            for (const auto& w : warnings)
                if (w.find(s) != std::string::npos) return "a warning contains a secret value";
        }
        return "";
    };
    if (c.contains("expect")) {
        try {
            json got = contract.load(env).to_json();
            for (const auto& [k, want] : c["expect"].items()) {
                if (!got.contains(k)) return k + ": missing from the result";
                if (!same(got[k], want)) return k + ": got " + got[k].dump() + ", want " + want.dump();
            }
            for (const auto& [k, v] : got.items())
                if (!c["expect"].contains(k)) return k + ": in the result but not expected";
            return leaked("");
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
        if (auto l = leaked(e.what()); !l.empty()) return l;
        if (got != want) return std::string("got errors:\n") + e.what();
        return "";
    }
}

TEST(Conformance, Cases) {
    std::string path = cases_path();
    std::ifstream f(path);
    if (!f) {
        if (env_is("DOCUCONF_REQUIRE_CONFORMANCE", "1")) FAIL() << "conformance cases not found at " << path;
        GTEST_SKIP() << "conformance cases not found at " << path << " (set DOCUCONF_CONFORMANCE)";
    }
    json doc = json::parse(f);
    ASSERT_EQ(doc.value("version", 0), 1) << path << ": unsupported suite version";
    ASSERT_FALSE(doc["cases"].empty()) << path << " holds no cases";
    int passed = 0, skipped = 0;
    std::vector<std::string> failed;
    std::map<std::string, int> skipped_tags;
    for (const auto& c : doc["cases"]) {
        bool skip = false;
        for (const auto& t : c.value("requires", json::array())) {
            if (!kSupported.count(t.get<std::string>())) {
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
            failed.push_back(c["id"].get<std::string>() + " (" + c.value("source", "") + "): " + problem);
        }
    }
    std::ostringstream summary;
    summary << "conformance: " << passed << " passed, " << skipped << " skipped, " << failed.size()
            << " failed, of " << doc["cases"].size() << " cases";
    for (const auto& [t, n] : skipped_tags) summary << " (skipped " << n << " requiring " << t << ")";
    std::cout << summary.str() << std::endl;
    for (const auto& m : failed) ADD_FAILURE() << m;
    if (skipped > 0 && env_is("DOCUCONF_REQUIRE_NO_SKIPS", "1"))
        ADD_FAILURE() << "docuconf-cpp must run every case, but skipped " << skipped << ": " << summary.str();
}

}  // namespace
