# docuconf for C++

Documentation: [docuconf.dev](https://docuconf.dev) · [C++ guide](https://docuconf.dev/languages/cpp/)

Typed configuration contracts for C++ services. Declare your service's environment variables and file inputs
once, next to the [CLI11](https://github.com/CLIUtils/CLI11) app you already have, and the same declaration
becomes a contract that your Kubernetes platform checks **before deploy** and your service checks again **at
boot**. It covers environment variables and file inputs: TLS key pairs, CA bundles, PKCS#12 and JKS keystores,
JSON/YAML/TOML config files, text and binary files.

Part of [docuconf](https://github.com/docuconf). See the
[specification](https://github.com/docuconf/docuconf-go/blob/main/spec/SPEC.md).

**Example:** [`examples/orders/`](examples/orders/), a small HTTP service with its declaration, exported contract
and boot-time errors.

> **Status:** `0.2.0`, not released yet. The contract format is a draft (`v1alpha1`) and the API may change. <!-- x-release-please-version -->
> Licensed under the [MIT licence](LICENSE).

## 1. Install

You need a C++17 compiler, CMake 3.16+ and git. Add docuconf to your `CMakeLists.txt` with `FetchContent`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(billing LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(docuconf
  GIT_REPOSITORY https://github.com/docuconf/docuconf-cpp.git
  GIT_TAG main)  # no release yet: pin a commit SHA for reproducible builds
FetchContent_MakeAvailable(docuconf)

add_executable(billing main.cpp)
target_link_libraries(billing PRIVATE docuconf::docuconf)
```

There is no release tag yet, so `GIT_TAG main` (or a commit SHA) is the install line until the first release;
the first release will add a `vX.Y.Z` tag and this section will use it (see [RELEASING.md](RELEASING.md)).

Dependencies come from, in order: targets your project already has (for example your own `FetchContent` of
CLI11), `find_package` (system packages, vcpkg, Conan), or a fetch at a pinned tag. CLI11, nlohmann/json, RE2,
json-schema-validator, yaml-cpp and toml++ are all fetched when missing, so on a bare machine the only package
you install is **OpenSSL 3** (`libssl-dev`, `openssl-devel`, `openssl-dev` or `brew install openssl@3`), which
the TLS and keystore checks need.

A service that reads only environment variables can skip OpenSSL, yaml-cpp and toml++ with the
environment-only build. Put this before `FetchContent_MakeAvailable(docuconf)`:

```cmake
set(DOCUCONF_FILE_INPUTS OFF CACHE BOOL "environment variables only")
```

Other options: `DOCUCONF_FETCH_DEPS=OFF` never fetches (configure fails instead, naming what is missing). To
install the library instead, `cmake -S . -B build && cmake --build build && cmake --install build`, then
`find_package(docuconf 0.1 REQUIRED)`. A top-level build defaults to `CMAKE_BUILD_TYPE=Release`. Installing
needs CLI11, nlohmann/json, RE2, yaml-cpp and toml++ from `find_package`, because the installed package config
looks for them the same way. json-schema-validator is the exception: when docuconf fetched it, it is installed
into the same prefix. When anything else was fetched, configure prints `docuconf: no install rules ...` and
skips them, or fails if you passed `-DDOCUCONF_INSTALL=ON`. On such a machine, use `FetchContent` instead.

## 2. Declare

`main.cpp`:

```cpp
#include <chrono>
#include <iostream>
#include <string>

#include <docuconf/docuconf.hpp>

int main(int argc, char** argv) {
    CLI::App app{"billing"};
    docuconf::Declaration config{app, "billing"};  // the contract's metadata.name

    int port = 0;
    config.add_var("PORT", port, "HTTP listen port").range(1, 65535).default_val(8080);

    std::string database_url;
    config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
        .secret()
        .schemes({"postgres", "postgresql"});

    std::chrono::milliseconds timeout{};
    config.add_var("REQUEST_TIMEOUT", timeout, "Upstream request timeout")
        .range(std::chrono::seconds(1), std::chrono::minutes(5))
        .default_val(std::chrono::seconds(30));

    DOCUCONF_PARSE(config, argc, argv);  // like CLI11_PARSE
    std::cout << "billing: listening on :" << port << ", timeout " << timeout.count() << "ms\n";
}
```

`add_var(name, target, description)` binds an environment variable to a C++ variable and returns a builder
whose methods chain with `.`. The C++ type picks the contract type (see [Types](#types)). A variable that is not
a `std::optional` and has no `default_val` is required.

### Descriptions and details

Every input needs a `description` (at least 5 characters) and may have `details`: CommonMark, used only in
generated docs and never at runtime, at most 4000 characters. C++ cannot read a comment at run time, so give
the input's doc comment to `.doc()`, in Doxygen style, with or without its `///` markers. Its first paragraph is
the description (on one line, a final period dropped) and the rest the details:

```cpp
std::uint16_t workers = 4;
config.add_var("WORKERS", workers)
    .doc(R"(
        /// Worker threads.
        ///
        /// Each worker holds one database connection, so keep this at or
        /// below the pool size, @c DB_POOL_SIZE. See @ref Pool::size.
        ///
        /// @li one connection per worker
        /// @li plus one for migrations
    )")
    .range(1, 64)
    .default_val(4);
```

exports

```cue
WORKERS: {
	type: "int"
	description: "Worker threads"
	details: "Each worker holds one database connection, so keep this at or\nbelow the pool size, `DB_POOL_SIZE`. See `Pool::size`.\n\n- one connection per worker\n- plus one for migrations"
	...
```

Doxygen becomes CommonMark: `@c`, `@p`, `@ref`, `<tt>` and `<code>` become code spans, `@a`/`@e` emphasis and
`@b` strong, `@code{.lang}`...`@endcode` and `@verbatim` fenced code blocks, `@li` list items, `@note` and
`@warning` bold labels, and `@param`, `@return` and other function tags are dropped. Markdown in the comment is
kept as written. `docuconf::split_doc()` is the same split, on its own. A comment that starts with a list or a
code block is all description. The explicit forms are the third argument of `add_var`/`add_file` (or
`.description()`) and `.details("...")`. Declaration and export fail when an input has no description, or
details that are blank or over 4000 characters (Unicode code points). Contract-first mode accepts `details` and
ignores them.

`docuconf docs` (in the [docuconf CLI](https://github.com/docuconf/docuconf-go)) generates CONFIG.md and
CONFIG.agents.md from the exported contract; the SDK only exports the text.

## 3. Run

```sh
cmake -S . -B build && cmake --build build
DATABASE_URL=postgres://billing:secret@localhost:5432/billing ./build/billing
```

```text
billing: listening on :8080, timeout 30000ms
```

`./build/billing --help` lists every variable in an `Environment variables` section, with its type, default,
and whether it is required or secret:

```text
Environment variables:
  PORT             HTTP listen port [int, default "8080"]
  DATABASE_URL     Primary Postgres connection string [url, REQUIRED, secret]
  REQUEST_TIMEOUT  Upstream request timeout [duration, default "30s"]
```

## 4. See an error

Every problem is reported at once, one line each, with a stable code. Secret values never appear:

```text
$ PORT=0 DATABASE_URL=mysql://billing:secret@db/billing REQUEST_TIMEOUT=30 ./build/billing
docuconf: 3 configuration problems:
  PORT: 0 is below min 1 (out_of_range)
  DATABASE_URL: value has scheme mysql, not one of postgres, postgresql (invalid_scheme)
  REQUEST_TIMEOUT: "30" is not a duration such as "1m30s" (invalid_type)
$ echo $?
1
```

The same lines go to `/dev/termination-log` when it exists (or to `DOCUCONF_TERMINATION_LOG`), so
`kubectl describe pod` shows them. A variable that is set but not declared and is close to a declared name gets
a warning, never with its value:

```text
docuconf: DATABSE_URL is set but not declared; did you mean DATABASE_URL?
```

Exit codes of `DOCUCONF_PARSE`:

| Code | When |
|---|---|
| 0 | after `--help` or `--docuconf-export` |
| 1 | configuration problems (above), or a contract that cannot be written |
| 2 | a mistake in the declaration itself (see [Declaration mistakes](#declaration-mistakes)) |
| CLI11's | a bad command line, such as an unknown flag (`app.exit` decides, as with `CLI11_PARSE`) |

## 5. Test your config

Put the declaration in a function that both `main` and the tests call, and load it from an explicit
environment map in tests: `load(env)` never reads or changes the process environment, starts no threads, and
reads `DOCUCONF_FILE_ROOT` (prepended to every absolute file path) from the map too. With GoogleTest:

```cpp
#include <gtest/gtest.h>

#include <docuconf/docuconf.hpp>

struct Config {
    int port = 0;
    std::string database_url;
};

void declare(docuconf::Declaration& d, Config& c) {
    d.add_var("PORT", c.port, "HTTP listen port").range(1, 65535).default_val(8080);
    d.add_var("DATABASE_URL", c.database_url, "Primary Postgres connection string").secret().schemes({"postgres"});
}

TEST(Config, UsesDefaults) {
    CLI::App app;
    docuconf::Declaration d{app, "billing"};
    Config c;
    declare(d, c);
    d.load({{"DATABASE_URL", "postgres://db/billing"}});
    EXPECT_EQ(c.port, 8080);
}

TEST(Config, RejectsPortZero) {
    CLI::App app;
    docuconf::Declaration d{app, "billing"};
    Config c;
    declare(d, c);
    try {
        d.load({{"PORT", "0"}, {"DATABASE_URL", "postgres://db/billing"}});
        FAIL() << "expected a ValidationError";
    } catch (const docuconf::ValidationError& e) {
        EXPECT_EQ(e.codes_for("PORT"), std::vector<docuconf::Code>{docuconf::Code::OutOfRange});
    }
}
```

`ValidationError` has `violations()`, `has(code)` and `codes_for(name)`. A mistake in the declaration throws
`docuconf::DeclarationError` from `load` and `check()`, so a test that only calls `d.check()` catches those.

## 6. Export the contract

The app exports its own contract, without reading the environment, so it works in a Dockerfile `RUN` step or
in CI:

```sh
./build/billing --docuconf-export contract.cue    # or --docuconf-export - for standard output
```

`config.export_cue()` returns the same text and `config.export_json()` its JSON form. The output starts with
`// Code generated by docuconf. DO NOT EDIT.`, is deterministic (variables and file inputs sorted by name) and
passes `cue vet -c` against the meta-schema. Commit it, and fail CI when a re-export differs from the committed
file, as [the example's CI does](.github/workflows/ci.yml).

## 7. Deploy

The platform validates what it intends to supply against `contract.cue` before anything is deployed: with
`docuconf vet` and `docuconf render` from [docuconf-go](https://github.com/docuconf/docuconf-go), or with the
Helm chart in [docuconf-go/helm](https://github.com/docuconf/docuconf-go/tree/main/helm). A missing
`DATABASE_URL` or `PORT: 0` is rejected at composition time, a secret must come from a Secret reference, and
the service checks the same rules again at boot.

---

## Reference

### File inputs

```cpp
#include <string>
#include <vector>

#include <docuconf/docuconf.hpp>

struct Route {
    std::string prefix;
    std::string upstream;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Route, prefix, upstream)
DOCUCONF_DEFINE_SCHEMA(Route, prefix, upstream)

struct Routes {
    std::vector<Route> routes;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Routes, routes)
DOCUCONF_DEFINE_SCHEMA(Routes, routes)

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    CLI::App app{"billing"};
    docuconf::Declaration config{app, "billing"};

    docuconf::TlsKeyPair tls;
    config.add_file("serving-tls", tls, "Certificate the service serves HTTPS with")
        .path("/etc/billing/tls")
        .required()
        .dns_names({"billing.internal"})
        .min_remaining(720h);

    docuconf::ConfigFile<Routes> routes;
    config.add_file("routes", routes, "Routing table: path prefixes and their upstreams")
        .path("/etc/billing/routes/routes.yaml")
        .path_env("ROUTES_FILE")
        .required();

    DOCUCONF_PARSE(config, argc, argv);
    // tls.certificate_pem, tls.key_pem and routes->routes are checked and loaded.
}
```

`add_file(name, target, description)` declares a file input; the target type picks the file type:
`TlsKeyPair` (`tls`), `CaBundle` (`caBundle`), `Keystore` (`keystore`), `TextFile` (`text`), `BinaryFile`
(`binary`), `ConfigFile<T>` (`config`; the format comes from the extension or `.format(...)`). File methods:
`path` (required), `path_env`, `required`, `secret`, `max_size`, `reload` (`"restart"`, or `"watch"` with a `Watched` target, below), `group`,
`deprecated`, `format`, `description`, `details`, `doc`, and per type `dns_names`, `key_algorithms`,
`min_remaining` (a `std::chrono` duration or Go syntax such as `"720h"`), `require_ca` (tls), `min_certificates` (caBundle), `password_var` (keystore),
`pattern`, `min_length`, `max_length` (text). `--help` lists file inputs in a `Files` section with their path
and `path_env`. The environment-only build (`DOCUCONF_FILE_INPUTS=OFF`) has no `add_file`.

A config file or `json` variable needs a JSON Schema, because C++ has no reflection.
`DOCUCONF_DEFINE_SCHEMA(Type, members...)`, next to nlohmann's `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` with the
same members, writes one from the member types: an object whose non-`std::optional` members are required, with
`bool`, integers (with their range), floating point, `std::string`, `std::vector`, `std::map<std::string, V>`,
`std::optional` and types that have a schema themselves. For anything richer, write a
`static nlohmann::json json_schema()` member or specialize `docuconf::json_schema<T>`. At boot the file is
checked against the schema **and** bound with `from_json`, so a schema that drifts from the type still fails at
boot (`schema_mismatch`).

File checks: the file exists, is readable and within `max_size`; config files parse (nlohmann/json, yaml-cpp with
the YAML 1.2 core schema, toml++; a UTF-8 byte-order mark is accepted), match their JSON Schema and bind to `T`;
TLS pairs (OpenSSL) parse, the key matches the certificate, the certificate is currently valid with at least
`min_remaining` left, covers every `dns_names` entry (a wildcard covers one label), uses an allowed key algorithm
and, with `require_ca`, chains to `ca.crt`; CA bundles hold at least `min_certificates` parseable certificates;
PKCS#12 keystores open with their `password_var` (an empty password when it is unset); text files match their
pattern and length limits.

Known gaps:

- **JKS keystores:** OpenSSL has no JKS parser, so docuconf checks the format and verifies the keystore's
  integrity digest (SHA-1 over the password and contents, as `keytool` writes it) with the password. A wrong
  password or corrupted file is `keystore_unreadable`; the entries themselves are not parsed.

### Reloading files (`reload: watch`)

A plain target is filled once, at boot (`reload: restart`: the platform rolls the pods when the source changes).
Bind the input to a `docuconf::Watched<T>` instead, with any of the target types above, and it is declared
`reload: watch` and reread while the app runs:

```cpp
docuconf::Watched<docuconf::TlsKeyPair> serving_tls;
config.add_file("serving-tls", serving_tls, "Certificate the service serves HTTPS with")
    .path("/etc/svc/tls")
    .required()
    .dns_names({"svc.internal"});
// After DOCUCONF_PARSE, on any thread, for example per TLS handshake:
//     std::shared_ptr<const docuconf::TlsKeyPair> pair = serving_tls.current();
```

- `current()` returns the current value as a `std::shared_ptr<const T>`, never null; a value it has handed out
  never changes, so keep the pointer while you use it. It is thread-safe, and readers never wait for a reload.
- At most once a second (`check_interval(...)` changes that), `current()` stats the input's files, following
  symlinks: the file, or a TLS directory's `tls.crt`, `tls.key` and `ca.crt`. Kubernetes updates a ConfigMap or
  Secret volume by swapping its `..data` symlink, so a swap shows up as a different file. `refresh()` checks now
  and returns whether a new value was loaded; `generation()` counts the values loaded (1 after boot).
- A changed file goes through every boot check. One that fails (a certificate that no longer covers its names,
  a config file that does not parse, a required file that disappeared) is not used: the previous value stays,
  and one warning per bad version goes to `on_warning`, naming the input and the violations and never a secret's
  content. The next change is checked again. An optional file that disappears becomes absent.
- `reload("watch")` on a plain target, or `reload("restart")` on a `Watched` one, is a declaration error.
- A keystore is reopened with the password variable's value read at boot: a running process's environment does
  not change, so the password is not read again. A new keystore that needs another password is
  `keystore_unreadable`, and the previous value stays. **Rotating a keystore's password needs a rollout**, which
  delivers the new keystore and the new password together.

Contract-first mode watches with `contract.watch(env)` (below), with the same hooks and status.

#### Using a watched value

A value copied once at startup never changes: a TLS server context, an HTTP client or a pool built from it keeps
the old certificate until it expires, even after the renewed one is mounted. Either read `current()` at each use,
or rebuild the long-lived object in an on-change hook:

- `on_change(fn)` registers `fn`, called with the new `std::shared_ptr<const T>` after a changed input passes its
  checks and replaces the current value, never for a change that fails them. It returns a
  `docuconf::WatchSubscription`; `cancel()` removes the hook (dropping it does not). Several hooks run in
  registration order, one reload at a time. A hook that throws is reported to `on_warning` by input name and
  exception type only (never `what()`); the new value stays and the other hooks run.
- While a hook is registered, a background thread checks the files once per `check_interval` (at least 10ms
  apart), so hooks run even when nothing calls `current()`. A `current()` or `refresh()` that finds the change
  first runs the hooks on its own thread instead. Without hooks there is no thread: changes are found by
  `current()` and `refresh()` alone.
- `reload_status()` returns a `docuconf::ReloadStatus` for a health check or a metric: `generation` (1 after boot,
  one more per accepted reload), `last_reload` (the `system_clock` time of the last accepted reload, empty while the
  boot value is current) and `last_rejected` (a `RejectedReload` with the `time`, the `input` and the violation
  `codes` of the last change that failed its checks, never its content; cleared by the next accepted change).

A TLS server that picks its certificate on every handshake, and an HTTP client whose TLS context, trusting a CA
bundle, is rebuilt when the bundle changes (OpenSSL):

```cpp
// (needs file inputs)
#include <docuconf/docuconf.hpp>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <memory>
#include <mutex>

docuconf::Watched<docuconf::TlsKeyPair> serving_tls;
docuconf::Watched<docuconf::CaBundle> upstream_ca;

// TLS server: SSL_CTX_set_cert_cb runs this on every handshake, so a renewed
// certificate is served from the next connection on.
int use_current_certificate(SSL* ssl, void*) {
    std::shared_ptr<const docuconf::TlsKeyPair> pair = serving_tls.current();
    BIO* cb = BIO_new_mem_buf(pair->certificate_pem.data(), static_cast<int>(pair->certificate_pem.size()));
    BIO* kb = BIO_new_mem_buf(pair->key_pem.data(), static_cast<int>(pair->key_pem.size()));
    X509* cert = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
    EVP_PKEY* key = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
    int ok = cert && key && SSL_use_certificate(ssl, cert) == 1 && SSL_use_PrivateKey(ssl, key) == 1;
    X509_free(cert);
    EVP_PKEY_free(key);
    BIO_free(cb);
    BIO_free(kb);
    return ok;
}

// HTTP client: a TLS context that trusts the bundle's certificates.
std::shared_ptr<SSL_CTX> client_context(const docuconf::CaBundle& ca) {
    std::shared_ptr<SSL_CTX> ctx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    BIO* b = BIO_new_mem_buf(ca.pem.data(), static_cast<int>(ca.pem.size()));
    while (X509* c = PEM_read_bio_X509(b, nullptr, nullptr, nullptr)) {
        X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), c);
        X509_free(c);
    }
    BIO_free(b);
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    return ctx;
}

std::mutex upstream_mutex;
std::shared_ptr<SSL_CTX> upstream;  // each new connection: SSL_new(upstream_context().get())

std::shared_ptr<SSL_CTX> upstream_context() {
    std::lock_guard<std::mutex> lock(upstream_mutex);
    return upstream;
}

int main(int argc, char** argv) {
    CLI::App app{"svc"};
    docuconf::Declaration config{app, "svc"};
    config.add_file("serving-tls", serving_tls, "Certificate the service serves HTTPS with")
        .path("/etc/svc/tls")
        .required();
    config.add_file("upstream-ca", upstream_ca, "CAs that sign the upstream API's certificate")
        .path("/etc/svc/upstream-ca/ca.crt")
        .required();
    DOCUCONF_PARSE(config, argc, argv);

    std::shared_ptr<SSL_CTX> server(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    SSL_CTX_set_cert_cb(server.get(), use_current_certificate, nullptr);

    upstream = client_context(*upstream_ca.current());
    upstream_ca.on_change([](std::shared_ptr<const docuconf::CaBundle> ca) {
        auto fresh = client_context(*ca);
        std::lock_guard<std::mutex> lock(upstream_mutex);
        upstream = std::move(fresh);  // connections already open keep the old context
    });

    // A health check can report the reload state, never the content:
    docuconf::ReloadStatus status = serving_tls.reload_status();
    if (status.last_rejected) { /* status.last_rejected->input, ->codes, ->time */ }
}
```

### Types

| Target type | Contract |
|---|---|
| `std::string` | `string`; `url` with `.url()` or `.schemes({...})`; `enum` with `.values({...})` |
| `int`, `std::uint16_t`, ... `std::int64_t` | `int`, with the type's range exported as `min`/`max` when narrower than 64 bits |
| `double`, `float` | `float` (NaN and infinity rejected) |
| `bool` | `bool` (`true`/`false`, any case) |
| `std::chrono::duration<...>` | `duration`, encoding `go` (`1m30s`) |
| a C++ `enum` with `.values({{"debug", Level::Debug}, ...})` | `enum` |
| `std::vector<std::string>`, `std::vector<std::uint16_t>`... | `list`, encoding `csv`; an int item type narrower than 64 bits exports its range as `itemMin`/`itemMax` |
| `docuconf::KeySet` | `keySet`, encoding `csv`, always secret |
| any other `T` with a JSON Schema and nlohmann's `from_json` | `json`, with that schema |
| `std::optional<T>` | optional, with no default |

Variable methods: `default_val`, `required`, `secret`, `flag`, `min`, `max`, `range`, `min_length`,
`max_length`, `pattern` (RE2, matches anywhere: anchor it with `^`/`$`), `url`, `schemes`, `values`,
`min_items`, `max_items`, `item_min`, `item_max`, `item_range`, `item_min_length`, `item_max_length`,
`delimiter` (the csv separator), `min_keys`, `max_keys`, `key_min_length`, `key_max_length`, `key_length`, `group`,
`examples`, `deprecated`, `config_key`, `description`, `details`, `doc`. A method that does not apply to the variable's type
does not compile (`min_length()` applies to a std::string variable). Durations take `std::chrono` values or Go
syntax strings (`"30s"`).

`deprecated("Use PORT instead", "PORT")` marks a variable or file input for staged removal (SPEC §4.2): the
message says what to use instead, is not blank and has at most 500 characters, and the second argument, the
optional `replacedBy`, names the input that replaces it. A required input cannot be deprecated, since the
platform could not stop setting it. A deprecated input that is set still loads and is still checked; at boot
docuconf logs a warning naming the input and the message, never the value, through `on_warning`.

`item_min`/`item_max` bound each item of an int list; an item outside them is `out_of_range` at boot. They are
narrowed to the item type, so `std::vector<std::uint16_t>` always exports `itemMin: 0, itemMax: 65535` or
tighter:

```cpp
std::vector<std::uint16_t> shards;
config.add_var("SHARDS", shards, "Shard ids this instance owns").item_range(0, 1023).default_val({});
```

| Constraint | Applies to |
|---|---|
| `min_length`, `pattern` | `string` |
| `max_length` | `string`, `url` (the URL as is), `json` (the value as received, before parsing, whitespace included; a default as compact JSON) |
| `item_min_length`, `item_max_length` | each item of a `std::vector<std::string>`, after splitting, so a separator never counts |
| `item_min`, `item_max`, `item_range` | each item of an int list |

Lengths count characters, meaning Unicode code points (UTF-8 lead bytes), never bytes: `日本` is 2 characters and
`ZÜ01` fits an `item_max_length(4)`. A value out of bounds is `out_of_range`; a secret is reported by its length,
never its value. Item lengths on an int list, or a length method on another type, do not compile; a `min_length`
or `pattern` on a `url` and `item_min_length` above `item_max_length` are declaration errors.

### Key sets

A `docuconf::KeySet` is a `keySet` (SPEC §4.3): secret keys that are all valid at once, so one can be rotated
without an outage ([spec section 6.1](https://github.com/docuconf/docuconf-go/blob/main/spec/SPEC.md#61-rotation)).
It is for the side that verifies: webhook signatures, inbound API keys, JWT HMAC verification.

```cpp
docuconf::KeySet webhook_keys;
config.add_var("WEBHOOK_KEYS", webhook_keys, "Keys that verify webhook signatures").key_length(32, 256);
// after DOCUCONF_PARSE:
bool known = webhook_keys.contains("candidate-api-key");     // constant time, every key compared
bool signed_ok = webhook_keys.verify([&](std::string_view key) {  // tries every key, even after a match
    return !key.empty();  // compare an HMAC under `key` here, in constant time (CRYPTO_memcmp)
});
(void)known;
(void)signed_ok;
```

The platform supplies it like a secret list, `WEBHOOK_KEYS=old,new` while a key is rotated. A key set is always
secret: it has no default, cannot be a flag, and prints as `***` through `operator<<`, `to_string` and
nlohmann::json; `keys()` returns the keys in the order the platform gave them. `min_keys` (default 1) and
`max_keys` (default 2) bound the number of keys (`too_few_items`, `too_many_items`), and `key_min_length` and
`key_max_length` each key's length (`out_of_range`). Keys are never trimmed, and an empty key, from a stray
separator, is always `out_of_range` with the message `key N is empty`, `N` counting from 1 as the keys were
received (`old,` has an empty key 2, `,new` an empty key 1). No message holds a key: one names a key by its position. The [orders example](examples/orders/README.md#rotate-a-key)
verifies webhooks with one, and the generated docs print the rotation steps for every key set.

### Command-line flags

Variables are environment-only by default: the platform validates the environment before deploy (SPEC §1.2),
and a flag would let a value it never saw win at boot. For local development, `.flag()` also accepts a
variable on the command line:

```cpp
int port = 0;
config.add_var("PORT", port, "HTTP listen port").range(1, 65535).default_val(8080).flag();  // --port

bool debug = false;
config.add_var("DEBUG_ENDPOINTS", debug, "Serve /debug").default_val(false).flag();  // --debug-endpoints, --no-debug-endpoints

std::vector<std::string> origins;
config.add_var("ALLOWED_ORIGINS", origins, "CORS origins").default_val({}).flag("-o,--origin");  // -o a b -o c
```

A flag behaves like a CLI11 option: it wins over the environment, a `bool` is a CLI11 flag (`--x`, `--no-x`,
`--x=false`), a list takes several arguments (each also split on the delimiter), and `.group(...)` is its
`--help` group. CLI11 only captures the raw text; docuconf parses and checks it like the environment value, and a
violation names the flag: `PORT (--port): 0 is below min 1 (out_of_range)`. A secret can never be a flag (it
would show in `ps` and shell history): `.secret().flag()` is a declaration error.

### Adopting in an existing CLI11 app

Keep your own options as they are; docuconf adds its variables next to them and only touches the app to add
`--docuconf-export`, the `--help` footer and the flags you ask for. To move an option that reads the
environment to docuconf, replace it:

```cpp
// before: CLI11
int port = 0;
app.add_option("--port", port, "HTTP listen port")->envname("PORT")->check(CLI::Range(1, 65535))->default_val(8080);
```

```cpp
// after: docuconf, with the flag kept for local runs
int port = 0;
config.add_var("PORT", port, "HTTP listen port").range(1, 65535).default_val(8080).flag();
```

`add_var` returns docuconf's builder, not a `CLI::Option*`, so CLI11's `->check(...)`, `->default_val(...)` and
`->envname(...)` cannot be attached to a variable by mistake (they would apply to the command line only and
skip the environment). Use docuconf's constraints instead. Declaring a flag whose name is already a CLI11 option,
or a variable twice, is a declaration error naming both, not a crash. CLI11 config files (`app.set_config`) are
rejected at declaration time: values read from them would bypass the contract. A footer callback you set with
`app.footer(std::function)` after creating the `Declaration` replaces docuconf's environment listing; a plain
`app.footer("...")` string is kept below it.

### Declaration mistakes

Mistakes in the declaration are a `docuconf::DeclarationError` (exit code 2 from `DOCUCONF_PARSE`), raised before
any value is read, one line per problem naming the variable: a name that is not `^[A-Z][A-Z0-9_]*$`, a short
description (under 5 characters), a default outside its own constraints, a duration bound or default that is not
a Go duration, a pattern RE2 cannot compile (lookaround, backreferences), a secret with a default or a flag, a
name declared twice or a flag that clashes with a CLI11 option, a file mounted over `/etc` or sharing a
directory, a `pathEnv` that is also a variable, a `passwordVar` that is not a secret, `reload("watch")` without a
`Watched` target, a `deprecated` message that is blank or over 500 characters, a required input that is deprecated (the platform
could not stop setting it), a key set that is not secret or whose `max_keys` is below `min_keys`, and `add_var`
after the configuration was loaded. A name such as `ENABLE_X` or `FF_X` gets a feature-flag warning
(SPEC §10).

### How values are parsed

docuconf parses every value itself, with the exact rules of SPEC §5, because CLI11's conversions (and C++'s
`strtol`, `strtod` and `from_chars`) accept more than the spec. Every SDK accepts exactly the same strings, and
anything else is `invalid_type`:

- **Values are never trimmed.** `" true"`, `"8080\n"` and `"5s "` fail, and so do non-ASCII digits.
- **Empty values:** an empty string is a present value for a `string` and unset for every other type.
- **Booleans:** `true` or `false` in any case (`TRUE`, `False`); not `1`, `0`, `t`, `yes`, `on` or `off`.
- **Integers:** `^[+-]?[0-9]+$`, base 10, 64-bit (`out_of_range` beyond). `+5` is 5 and `010` is ten, never
  octal; `0x10`, `1_000`, `1e3` and `5.0` are `"0x10" is not a base-10 integer`.
- **Floats:** `^[+-]?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?$`, locale-independent: not `.5`, `5.`, `inf`, `NaN`, a
  hex float, `0,5`, or a value too large for a double (`1e400`).
- **Durations:** Go syntax (`1m30s`, `1.5h`, `-5s`, `0`), exactly as Go's `time.ParseDuration` reads it, so the
  contract says `encoding: "go"`; a bare number, `5S`, `1d` and `1m 30s` fail. Contract-first mode also reads
  `iso8601` (`PT1,5S`, `P1DT2H`), `seconds` (`90`, `0.25`) and `timespan` (`1.02:03:04.5`), each by its own grammar.
- **Lists and key sets:** split on every occurrence of the declared separator, keeping every item as it is, so
  `a, b` is `a` and ` b` and `a,,b` has an empty middle item; the contract says `encoding: "csv"`.

The declaration API has no profiles or config-file overlays (SPEC §4.4, §4.7), since CLI11 does not layer
config files, so a contract exported from C++ never has them; contract-first mode reads both. A secret that still holds an injector reference (`vault:`, `op://`, `ref+`) is `invalid_type`,
naming the scheme and never the value.

### Contract-first mode

Validate an environment against a contract given as JSON (`cue export contract.cue`), with no C++ declaration:

```cpp
auto contract = docuconf::Contract::from_json(R"({
  "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "billing"},
  "vars": {
    "PORT": {"type": "int", "description": "HTTP listen port", "default": 8080},
    "TIMEOUT": {"type": "duration", "encoding": "iso8601", "description": "Request timeout", "default": "PT30S"}
  }
})");
docuconf::Values values = contract.load({{"PORT", "9090"}, {"TIMEOUT", "PT1.5S"}});
std::int64_t contract_port = values.get("PORT")->as_int();
std::cout << values << "\n";  // secrets print as "***"
(void)contract_port;
```

It parses every encoding in SPEC §5 (`csv`, `json` and `indexed` lists and key sets; `go`, `iso8601`,
`seconds` and `timespan` durations) and checks values with the same code as the declaration path.
`load_process_env()` reads the process environment, warns about likely typos and writes the termination log on
failure. `values.key_set("NAME")` returns a key set as a `docuconf::KeySet`.

The rest of a contract is honoured too, through the same checks as the declaration API:

- **File inputs** (`config` in `json`, `yaml` or `toml`, `tls`, `caBundle`, `keystore` (PKCS#12, through
  OpenSSL 3), `text` and `binary`), read from under `DOCUCONF_FILE_ROOT` when the environment sets it.
  `values.file("name")` returns a `docuconf::FileValue`; `to_json()` shows a config file as its data, a text
  file as its text and any other present file as `true`.
- **Profiles and config-file overlays** (SPEC §4.4, §4.7), layered in this order: the variable's default, the
  selected profile's default, an overlay (read as an optional file at its `path`, under `DOCUCONF_FILE_ROOT`,
  each value at its `configKey`), then the environment. A secret is never read from an overlay, and an overlay
  that does not parse is `file_malformed` for the overlay.
- **`reload: watch`**, for file inputs and overlays: `contract.watch(env)` loads like `load(env)` and returns a
  `docuconf::Watched<docuconf::Values>`. Its `current()` reloads the contract when a watched input's files
  change, as above: the environment and the inputs declared `restart` keep their boot values, and a reload that
  fails its checks keeps the previous `Values` and warns. `on_change` and `reload_status()` work as for a file
  input; a rejected change's `input` names every input with a violation. `load(env)` reads every input once,
  whatever its `reload`: use `watch(env)` to honour `reload: watch`.
- **Warnings** go to `contract.on_warning(...)` (standard error by default): a deprecated input that is set, a
  variable set both in the environment and in an overlay, and a watched change that failed its checks. They name the input and the message, never the value.

The environment-only build (`DOCUCONF_FILE_INPUTS=OFF`) rejects a contract with file inputs or overlays.

### Conformance

The test suite runs the shared conformance cases (SPEC §12) through contract-first mode. It reads
`cases.json` from `DOCUCONF_CONFORMANCE`, falling back to `../docuconf-go/conformance/cases.json` next to this
repository, and fails instead of skipping when `DOCUCONF_REQUIRE_CONFORMANCE=1` and the file is missing:

```sh
DOCUCONF_CONFORMANCE=../docuconf-go/conformance/cases.json DOCUCONF_REQUIRE_CONFORMANCE=1 \
  ./build/tests/docuconf_tests --gtest_filter='Conformance.*'
```

The runner keeps an allow-list of the capability tags it supports and skips a case with any other tag, so a tag
added to the suite later never breaks it. It supports every tag, so **no case is skipped**: integers are
`std::int64_t` (`int64`), `json` values are checked against their JSON Schema (`json-schema`), and it has key sets
(`key-set`), deprecated inputs (`deprecated`), the exact parsing rules (`strict-parsing`), file inputs (`files`,
including PKCS#12 keystores with AES-256 and a SHA-256 MAC), profiles (`profiles`) and overlays (`overlays`).
Each case's files are written under a new directory, set as `DOCUCONF_FILE_ROOT`. With
`DOCUCONF_REQUIRE_NO_SKIPS=1`, as in CI, a skipped case fails the run. The environment-only build cannot read
files, so it skips the `files` and `overlays` cases. Failures are reported by case id.

The shared export fixture (`conformance/export/fixture.yaml`) is declared in
[`tests/export_fixture_test.cpp`](tests/export_fixture_test.cpp), exported, and compared with
`conformance/export/golden.cue` by `docuconf conformance export` (the CLI from `DOCUCONF_CLI` or `PATH`), and
must match it exactly; its `settings` and `serving-tls` are `Watched` targets, so they export `reload: watch`. The
gateway golden contract in `tests/golden/gateway.cue` stays as well.

### Development

```sh
cmake -S . -B build -G Ninja
cmake --build build
DOCUCONF_SPEC_CUE=../docuconf-go/spec/cue ctest --test-dir build --output-on-failure
UPDATE_GOLDEN=1 ./build/tests/docuconf_tests --gtest_filter='Export.MatchesGolden'   # after an intended change
```

ctest also builds every C++ block of this README (`tests/readme_snippets.py`), runs the testing example above,
and checks that the declaration mistakes the compiler should catch do not compile (`tests/compile_fail/`). The
export tests run `cue vet -c` on the golden contract against the meta-schema in `DOCUCONF_SPEC_CUE` (default
`../docuconf-go/spec/cue`) with the `cue` binary from `CUE`, `~/go/bin/cue` or `PATH`, and skip when either is
missing.

## Licence

MIT. See [LICENSE](LICENSE).
