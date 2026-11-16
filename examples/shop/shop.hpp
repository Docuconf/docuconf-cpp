// shop: one program, two roles (web and worker), configured contract-first
// from contract.json.
#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include <docuconf/docuconf.hpp>

namespace shop {

/// contract.json, compiled in.
const char* contract_json();

/// The contract, read from contract_json() on first use.
const docuconf::Contract& contract();

/// The typed configuration the app uses. Inputs of the other role keep
/// their defaults or stay empty.
struct Config {
    std::string role;  // "web" or "worker"
    std::int64_t port = 0;
    std::vector<std::string> allowed_origins;
    std::vector<std::string> queues;
    bool webhooks_enabled = false;
};

/// Reads the Config out of loaded Values.
Config to_config(const docuconf::Values& values);

/// The line the app prints once its configuration is valid:
/// `shop web: port 8080, origins https://shop.example` or
/// `shop worker: queues default,mail, webhooks off`.
std::string summary(const Config& config);

/// Loads the configuration with `load` and prints summary() to `out`
/// (exit code 0), or every violation to `err` (exit code 1). An invalid
/// contract is exit code 2.
int boot(const std::function<docuconf::Values()>& load, std::ostream& out, std::ostream& err);

}  // namespace shop
