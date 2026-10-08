// Must not compile. Expected: item_max_length() applies to a std::vector<std::string>
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    std::vector<std::int64_t> ints;
    config.add_var("INTS", ints, "Item lengths on ints").item_max_length(3);
}
