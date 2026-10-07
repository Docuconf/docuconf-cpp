// Must not compile. Expected: max_length() applies to a std::string (string or url) or json variable
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    int n = 0;
    config.add_var("N", n, "Max length on an int").max_length(3);
}
