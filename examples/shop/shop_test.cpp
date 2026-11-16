// The shop test matrix (README.md): each case loads an environment through
// the contract, with a fresh, empty DOCUCONF_FILE_ROOT, and checks the exit
// code, the printed line or the violations. Run by CI after the build.
#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "shop.hpp"

namespace fs = std::filesystem;

namespace {

const std::string kDatabaseUrl = "postgres://shop@db:5432/shop";
const std::string kKeyPath = "etc/shop/webhooks/signing.key";  // under the file root

int failures = 0;

struct Problem {
    std::string input;
    docuconf::Code code;
    std::string message_contains = {};
};

struct Case {
    std::string name;
    docuconf::Env env;  // on top of DATABASE_URL
    std::string key_file = {};  // written at kKeyPath when not empty
    std::string line = {};  // the printed line when the config is valid
    std::vector<Problem> problems = {};  // exactly these, in any order
};

void fail(const Case& c, const std::string& what) {
    std::cerr << "FAIL " << c.name << ": " << what << "\n";
    ++failures;
}

void run(const Case& c) {
    std::string tmpl = (fs::temp_directory_path() / "shop-test-XXXXXX").string();
    fs::path root = ::mkdtemp(tmpl.data());  // fresh and empty
    if (!c.key_file.empty()) {
        fs::create_directories((root / kKeyPath).parent_path());
        std::ofstream(root / kKeyPath) << c.key_file;
    }
    docuconf::Env env = c.env;
    env["DATABASE_URL"] = kDatabaseUrl;
    env["DOCUCONF_FILE_ROOT"] = root.string();

    int before = failures;
    std::ostringstream out, err;
    int code = shop::boot([&] { return shop::contract().load(env); }, out, err);
    if (c.problems.empty()) {
        if (code != 0) fail(c, "exit code " + std::to_string(code) + ", want 0: " + err.str());
        if (out.str() != c.line + "\n") fail(c, "printed '" + out.str() + "', want '" + c.line + "'");
    } else {
        if (code != 1) fail(c, "exit code " + std::to_string(code) + ", want 1");
        if (!out.str().empty()) fail(c, "printed '" + out.str() + "' on stdout");
        if (err.str().find(kDatabaseUrl) != std::string::npos) fail(c, "the DATABASE_URL secret was printed");
        try {
            shop::contract().load(env);
            fail(c, "load() did not throw");
        } catch (const docuconf::ValidationError& e) {
            const auto& got = e.violations();
            if (got.size() != c.problems.size())
                fail(c, std::to_string(got.size()) + " violations, want " + std::to_string(c.problems.size()) +
                            ":\n" + e.what());
            for (const auto& want : c.problems) {
                bool found = false;
                for (const auto& v : got)
                    if (v.input == want.input && v.code == want.code &&
                        v.message.find(want.message_contains) != std::string::npos)
                        found = true;
                if (!found)
                    fail(c, std::string("no ") + want.input + " " + docuconf::to_string(want.code) +
                                (want.message_contains.empty() ? "" : " containing '" + want.message_contains + "'") +
                                " in:\n" + e.what());
            }
        }
    }
    fs::remove_all(root);
    std::cout << (failures == before ? "ok   " : "FAIL ") << c.name << "\n";
}

}  // namespace

int main() {
    using docuconf::Code;
    const std::string key(40, 'k');
    std::vector<Case> cases = {
        {"1 web", {{"DOCUCONF_ROLE", "web"}, {"ALLOWED_ORIGINS", "https://shop.example"}}, "",
         "shop web: port 8080, origins https://shop.example"},
        {"2 worker needs no ALLOWED_ORIGINS", {{"DOCUCONF_ROLE", "worker"}, {"QUEUES", "default,mail"}}, "",
         "shop worker: queues default,mail, webhooks off"},
        {"3 web without ALLOWED_ORIGINS", {{"DOCUCONF_ROLE", "web"}}, "", "",
         {{"ALLOWED_ORIGINS", Code::MissingRequired}}},
        {"4 no role", {{"QUEUES", "default"}}, "", "", {{"DOCUCONF_ROLE", Code::MissingRequired}}},
        {"5 unknown role", {{"DOCUCONF_ROLE", "cron"}}, "", "", {{"DOCUCONF_ROLE", Code::NotInEnum}}},
        {"6 LOG_FORMAT=json without LOG_SCHEMA_URL",
         {{"DOCUCONF_ROLE", "worker"}, {"QUEUES", "default"}, {"LOG_FORMAT", "json"}}, "", "",
         {{"LOG_SCHEMA_URL", Code::MissingRequired, R"(is required when LOG_FORMAT is "json")"}}},
        {"7 LOG_FORMAT=json with LOG_SCHEMA_URL",
         {{"DOCUCONF_ROLE", "worker"},
          {"QUEUES", "default"},
          {"LOG_FORMAT", "json"},
          {"LOG_SCHEMA_URL", "https://logs.example/v1.json"}},
         "", "shop worker: queues default, webhooks off"},
        {"8 webhooks without the signing key",
         {{"DOCUCONF_ROLE", "worker"}, {"QUEUES", "default"}, {"WEBHOOKS_ENABLED", "true"}}, "", "",
         {{"webhook-signing-key", Code::FileMissing, ", and is required when WEBHOOKS_ENABLED is true"}}},
        {"9 webhooks with the signing key",
         {{"DOCUCONF_ROLE", "worker"}, {"QUEUES", "default"}, {"WEBHOOKS_ENABLED", "true"}}, key,
         "shop worker: queues default, webhooks on"},
        {"10 web ignores the worker's signing key",
         {{"DOCUCONF_ROLE", "web"}, {"ALLOWED_ORIGINS", "https://shop.example"}, {"WEBHOOKS_ENABLED", "true"}}, "",
         "shop web: port 8080, origins https://shop.example"},
        {"11 fragment variables are checked at boot",
         {{"DOCUCONF_ROLE", "web"},
          {"ALLOWED_ORIGINS", "https://shop.example"},
          {"OTEL_EXPORTER_OTLP_PROTOCOL", "carrier-pigeon"}},
         "", "", {{"OTEL_EXPORTER_OTLP_PROTOCOL", Code::NotInEnum}}},
    };
    for (const auto& c : cases) run(c);
    std::cout << cases.size() << " cases, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
