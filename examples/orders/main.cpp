// orders: a small HTTP service whose configuration is a docuconf contract.
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include <httplib.h>

#include <docuconf/docuconf.hpp>

int main(int argc, char** argv) {
    CLI::App app{"orders: a small HTTP service configured with docuconf"};
    // The service name becomes the contract's metadata.name.
    docuconf::Declaration config{app, "orders"};

    // Each variable is read from its environment variable and checked at
    // boot; --help lists them all. PORT is also a command-line flag (--port)
    // for local runs; the platform only ever sets the environment.
    int port = 0;
    config.add_var("PORT", port, "HTTP listen port").range(1, 65535).default_val(8080).flag();

    std::string log_level;
    config.add_var("LOG_LEVEL", log_level, "Minimum log level emitted")
        .values({"debug", "info", "warn", "error"})
        .default_val("info");

    // A secret: never printed, never given a default, supplied by the
    // platform from a Kubernetes Secret.
    std::string database_url;
    config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
        .secret()
        .required()
        .schemes({"postgres"});

    std::vector<std::string> allowed_origins;
    config.add_var("ALLOWED_ORIGINS", allowed_origins, "Origins allowed to call the API (CORS)")
        .min_items(1)
        .default_val({"http://localhost:3000"});

    std::chrono::milliseconds request_timeout{};
    config.add_var("REQUEST_TIMEOUT", request_timeout, "Time allowed to read a request")
        .range("1s", "5m")
        .default_val("30s");

    int worker_count = 0;
    config.add_var("WORKER_COUNT", worker_count)
        .doc(R"(
            /// Number of request worker threads.
            ///
            /// Each worker answers one connection at a time, so this is also the
            /// number of requests served at once. Raise it when requests queue up.
            ///
            /// Keep it at or below the database pool size:
            /// @li one connection per worker;
            /// @li plus one for migrations.
        )")
        .range(1, 64)
        .default_val(4);

    // Parses, validates every input and binds the values, or exits:
    // 0 after --help or --docuconf-export, 1 with every violation listed,
    // 2 for a mistake in the declaration.
    DOCUCONF_PARSE(config, argc, argv);

    httplib::Server server;
    server.new_task_queue = [&] { return new httplib::ThreadPool(static_cast<std::size_t>(worker_count)); };
    auto timeout = std::chrono::duration_cast<std::chrono::seconds>(request_timeout);
    server.set_read_timeout(timeout.count(), 0);

    server.Get("/healthz", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });
    server.Get("/config", [&](const httplib::Request&, httplib::Response& res) {
        nlohmann::json body = {
            {"PORT", port},
            {"LOG_LEVEL", log_level},
            {"DATABASE_URL", "***"},
            {"ALLOWED_ORIGINS", allowed_origins},
            {"REQUEST_TIMEOUT", docuconf::format_go_duration(request_timeout)},
            {"WORKER_COUNT", worker_count},
        };
        res.set_content(body.dump(), "application/json");
    });

    std::cerr << "orders: listening on :" << port << std::endl;
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "orders: cannot listen on :" << port << std::endl;
        return 1;
    }
}
