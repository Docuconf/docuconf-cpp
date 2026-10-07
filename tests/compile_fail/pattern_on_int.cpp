// Must not compile. Expected: pattern() applies to a std::string variable
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    int x = 0;
    config.add_var("X", x, "An int value").pattern("^a");
}
