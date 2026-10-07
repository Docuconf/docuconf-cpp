// Must not compile. Expected: no member named.+option
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    int x = 0;
    config.add_var("X", x, "An int value").option();
}
