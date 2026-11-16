#include "shop.hpp"

namespace shop {

namespace {

std::vector<std::string> strings(const docuconf::Values& values, const std::string& name) {
    std::vector<std::string> out;
    if (const auto* v = values.get(name))
        for (const auto& item : v->as_list()) out.push_back(item.as_string());
    return out;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
    return out;
}

}  // namespace

const docuconf::Contract& contract() {
    static const docuconf::Contract c = docuconf::Contract::from_json(contract_json());
    return c;
}

Config to_config(const docuconf::Values& values) {
    Config c;
    c.role = values.get("DOCUCONF_ROLE")->as_string();
    if (const auto* v = values.get("PORT")) c.port = v->as_int();
    c.allowed_origins = strings(values, "ALLOWED_ORIGINS");
    c.queues = strings(values, "QUEUES");
    if (const auto* v = values.get("WEBHOOKS_ENABLED")) c.webhooks_enabled = v->as_bool();
    return c;
}

std::string summary(const Config& c) {
    if (c.role == "web")
        return "shop web: port " + std::to_string(c.port) + ", origins " + join(c.allowed_origins);
    return "shop worker: queues " + join(c.queues) + ", webhooks " + (c.webhooks_enabled ? "on" : "off");
}

int boot(const std::function<docuconf::Values()>& load, std::ostream& out, std::ostream& err) {
    try {
        out << summary(to_config(load())) << std::endl;
        return 0;
    } catch (const docuconf::ValidationError& e) {
        // Every violation, one per line; messages never hold a secret.
        err << e.what() << std::endl;
        return 1;
    } catch (const docuconf::DeclarationError& e) {
        err << e.what() << std::endl;
        return 2;
    }
}

}  // namespace shop
