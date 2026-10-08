// Must not compile. Expected: min_items() applies to a std::vector variable
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    int x = 0;
    config.add_var("X", x, "An int value").min_items(1);
}
