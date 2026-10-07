// Must not compile. Expected: min(), max() and range() apply to int, float and duration variables
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    std::string s;
    config.add_var("S", s, "A string value").range(1, 2);
}
