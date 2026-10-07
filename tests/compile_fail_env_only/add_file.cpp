// Must not compile. Expected: this build has no file inputs
#include <docuconf/docuconf.hpp>

int main() {
    CLI::App app;
    docuconf::Declaration config{app, "svc"};
    docuconf::TextFile license;
    config.add_file("license", license, "License key").path("/etc/svc/license.key");
}
