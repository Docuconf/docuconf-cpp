// Must not compile. Expected: item_min() applies to a std::vector of integers
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    std::vector<std::string> s;
    config.add_var("S", s, "A list of strings").item_min(1);
}
