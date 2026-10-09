// Must not compile. Expected: delimiter() applies to a std::vector or a docuconf::KeySet variable
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    std::string s;
    config.add_var("S", s, "A string value").delimiter(";");
}
