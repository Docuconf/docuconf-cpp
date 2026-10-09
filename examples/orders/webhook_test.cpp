// Tests the WEBHOOK_KEYS key set: a rotation, step by step, and the key
// sets the declaration stops at boot. Run by CI after the build.
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

#include "webhook.hpp"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

const std::string kOld(32, 'o');
const std::string kNew(32, 'n');
const std::string kBody = R"({"order":"42","status":"paid"})";

std::string sign(const std::string& key) {
    std::string mac = orders::hmac_sha256(key, kBody), hex;
    char buf[3];
    for (unsigned char c : mac) {
        std::snprintf(buf, sizeof buf, "%02x", c);
        hex += buf;
    }
    return hex;
}

// Loads WEBHOOK_KEYS as the service does at boot.
std::optional<docuconf::KeySet> load(const std::string& value) {
    CLI::App app;
    docuconf::Declaration config{app, "orders"};
    std::optional<docuconf::KeySet> keys;
    orders::declare_webhook_keys(config, keys);
    // No DOCUCONF_TERMINATION_LOG: load() then writes no termination log.
    config.load({{"WEBHOOK_KEYS", value}});
    return keys;
}

}  // namespace

int main() {
    // A rotation: each step is a rollout with a new WEBHOOK_KEYS, and a
    // webhook signed with the key in use always verifies.
    struct Step {
        const char* name;
        std::string keys;
        bool old_ok, new_ok;
    };
    for (const Step& s : {Step{"before", kOld, true, false}, Step{"overlap", kOld + "," + kNew, true, true},
                          Step{"after", kNew, false, true}}) {
        auto keys = load(s.keys).value();
        expect(orders::verify(keys, kBody, sign(kOld)) == s.old_ok, std::string(s.name) + ": old key");
        expect(orders::verify(keys, kBody, sign(kNew)) == s.new_ok, std::string(s.name) + ": new key");
        expect(!orders::verify(keys, kBody, sign(std::string(32, 'x'))), std::string(s.name) + ": other key");
    }
    docuconf::KeySet old_only({kOld});
    expect(!orders::verify(old_only, kBody, "not hex"), "accepted a malformed signature");
    expect(!orders::verify(old_only, kBody, ""), "accepted an empty signature");
    expect(!orders::verify(docuconf::KeySet{}, kBody, sign(kOld)), "accepted a webhook with no keys configured");
    expect(!load("").has_value(), "an empty WEBHOOK_KEYS is unset");

    // An empty or truncated key, or a third key, stops the service at boot,
    // and the error never prints a key.
    struct Bad {
        std::string value;
        docuconf::Code code;
    };
    for (const Bad& b : {Bad{kOld + ",", docuconf::Code::OutOfRange},
                         Bad{kOld + "," + kNew.substr(0, 10), docuconf::Code::OutOfRange},
                         Bad{kOld + "," + kNew + "," + std::string(32, 'x'), docuconf::Code::TooManyItems}}) {
        try {
            load(b.value);
            expect(false, "loaded a bad key set");
        } catch (const docuconf::ValidationError& e) {
            auto codes = e.codes_for("WEBHOOK_KEYS");
            expect(codes.size() == 1 && codes[0] == b.code, std::string("wrong code for a bad key set: ") + e.what());
            std::string text = e.what();
            expect(text.find(kOld) == std::string::npos && text.find(kNew.substr(0, 10)) == std::string::npos,
                   "the error printed a key: " + text);
        }
    }

    if (failures != 0) return 1;
    std::cout << "webhook_test: ok\n";
    return 0;
}
