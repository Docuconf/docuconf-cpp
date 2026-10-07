// Must not compile. Expected: has no member named .check.|no member named .check.
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    int x = 0;
    config.add_var("X", x, "An int value").check(CLI::Range(1, 10));
}
