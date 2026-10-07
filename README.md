# docuconf for C++

Typed configuration contracts for [CLI11](https://github.com/CLIUtils/CLI11). Declare your service's
environment variables and file inputs as CLI11 options with a little docuconf metadata, and the same declaration
becomes a contract that your Kubernetes platform checks **before deploy** and your service checks again **at
boot**. It covers environment variables and file inputs: TLS key pairs, CA bundles, PKCS#12 and JKS keystores,
JSON/YAML/TOML config files, text and binary files.

Part of [docuconf](https://github.com/docuconf). See the
[specification](https://github.com/docuconf/docuconf-go/blob/main/spec/SPEC.md).

**Example:** [`examples/orders/`](examples/orders/), a small HTTP service with its declaration, exported contract
and boot-time errors.

> **Status:** `0.1.0`. The contract format is a draft (`v1alpha1`) and the API may change. Licensed under the
> [MIT licence](LICENSE).

## Why CLI11

C++ has no single environment-config library the way Go or Python do. CLI11 is the most widely used modern C++
options library, and every option already has an environment binding (`->envname()`), a description, a type
name and a default for `--help`. docuconf builds on that: each variable is a CLI11 option, so it shows up in
`--help` with its environment name, and CLI11 still decides where a value comes from (the command line, then
the environment). docuconf adds what CLI11 lacks: the contract metadata (`secret`, constraints, file inputs),
the spec's parsing rules, all violations reported together, and export.

## Install

Requirements: a C++17 compiler, CMake 3.16+, and these libraries (all packaged on Ubuntu 24.04):

```sh
sudo apt-get install libcli11-dev nlohmann-json3-dev libre2-dev libssl-dev \
  libyaml-cpp-dev libtomlplusplus-dev libgtest-dev
```

[pboettch/json-schema-validator](https://github.com/pboettch/json-schema-validator) (JSON Schema checks of
config files and `json` variables) is found with `find_package` when installed, and otherwise fetched at a pinned
tag (2.4.0). `-DDOCUCONF_FETCH_DEPS=OFF` turns fetching off.

With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(docuconf
  GIT_REPOSITORY https://github.com/docuconf/docuconf-cpp.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(docuconf)
target_link_libraries(my_service PRIVATE docuconf::docuconf)
```

Or install it (`cmake --install build`) and use `find_package(docuconf 0.1 REQUIRED)`.

## Declare

```cpp
#include <docuconf/docuconf.hpp>

struct Routes {
    std::vector<Route> routes;
    static nlohmann::json json_schema();  // the JSON Schema of this type
};
void from_json(const nlohmann::json& j, Routes& r);  // nlohmann's usual binding

int main(int argc, char** argv) {
    CLI::App app{"billing"};
    docuconf::Declaration config{app, "billing"};  // metadata.name

    int port = 0;
    config.add_var("PORT", port, "HTTP listen port")->range(1, 65535)->default_val(8080);

    std::string database_url;
    config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
        ->secret()
        ->schemes({"postgres", "postgresql"});

    std::chrono::milliseconds timeout{};
    config.add_var("REQUEST_TIMEOUT", timeout, "Upstream request timeout")->range("1s", "5m")->default_val("30s");

    docuconf::TlsKeyPair tls;
    config.add_file("serving-tls", tls, "Certificate the service serves HTTPS with")
        ->path("/etc/billing/tls")
        ->required()
        ->dns_names({"billing.internal"})
        ->min_remaining("720h");

    docuconf::ConfigFile<Routes> routes;
    config.add_file("routes", routes, "Routing table: path prefixes and their upstreams")
        ->path("/etc/billing/routes/routes.yaml")
        ->path_env("ROUTES_FILE")
        ->required();

    DOCUCONF_PARSE(config, argc, argv);  // like CLI11_PARSE
    // port, database_url, timeout, tls and routes now hold checked values.
}
```

`add_var(name, target, description)` registers a CLI11 option (`PORT` becomes `--port`, with
`->envname("PORT")`) and returns a builder whose methods chain like CLI11's `Option*`. The description is
required (at least 5 characters). A variable whose target is not a `std::optional` and that has no
`default_val` is required. The C++ type picks the contract type:

| Target type | Contract |
|---|---|
| `std::string` | `string`; `url` with `->url()` or `->schemes({...})`; `enum` with `->values({...})` |
| `int`, `std::uint16_t`, ... `std::int64_t` | `int`, with the type's range exported as `min`/`max` when narrower than 64 bits |
| `double`, `float` | `float` (NaN and infinity rejected) |
| `bool` | `bool` (`true`/`false`, any case) |
| `std::chrono::duration<...>` | `duration`, encoding `go` (`1m30s`) |
| a C++ `enum` with `->values({{"debug", Level::Debug}, ...})` | `enum` |
| `std::vector<std::string>`, `std::vector<std::uint16_t>`... | `list`, encoding `csv`; an int item type narrower than 64 bits exports its range as `itemMin`/`itemMax` |
| any other `T` with a `json_schema()` and nlohmann's `from_json` | `json`, with that schema |
| `std::optional<T>` | optional, with no default |

Variable methods: `default_val`, `required`, `secret`, `min`, `max`, `range`, `min_length`, `max_length`,
`pattern` (RE2, matches anywhere: anchor it with `^`/`$`), `url`, `schemes`, `values`, `min_items`, `max_items`,
`item_min`, `item_max`, `item_range`, `delimiter` (the csv separator), `group`, `examples`, `deprecated`,
`config_key`, `description`. `option()` returns the underlying `CLI::Option*`.

`item_min`/`item_max` bound each item of an int list; an item outside them is `out_of_range` at boot. They are
narrowed to the item type, so `std::vector<std::uint16_t>` always exports `itemMin: 0, itemMax: 65535` or
tighter:

```cpp
std::vector<std::uint16_t> shards;
config.add_var("SHARDS", shards, "Shard ids this instance owns")->item_range(0, 1023)->default_val({});
```

`add_file(name, target, description)` declares a file input; the target type picks the file type:
`TlsKeyPair` (`tls`), `CaBundle` (`caBundle`), `Keystore` (`keystore`), `TextFile` (`text`), `BinaryFile`
(`binary`), `ConfigFile<T>` (`config`, where `T` has a `json_schema()` and `from_json`; the format comes from the
extension or `->format(...)`). File methods: `path` (required), `path_env`, `required`, `secret`, `max_size`,
`reload` (only `"restart"`), `group`, `deprecated`, `format`, and per type `dns_names`, `key_algorithms`,
`min_remaining`, `require_ca` (tls), `min_certificates` (caBundle), `password_var` (keystore), `pattern`,
`min_length`, `max_length` (text).

C++ has no reflection, so the JSON Schema of a config file or `json` variable is written next to the type, as a
`static nlohmann::json json_schema()` member or a `docuconf::json_schema<T>` specialization. At boot the file is
checked against that schema **and** bound with `from_json`, so a schema that drifts from the type still fails at
boot (`schema_mismatch`).

Mistakes in the declaration are a `docuconf::DeclarationError`, raised before any value is read: a name that is
not `^[A-Z][A-Z0-9_]*$`, a short description, a default outside its own constraints, a pattern RE2 cannot compile
(lookaround, backreferences), a file mounted over `/etc` or sharing a directory, a `pathEnv` that is also a
variable, a `passwordVar` that is not a secret, `reload("watch")`. A name such as `ENABLE_X` or `FF_X` gets a
feature-flag warning (SPEC §10).

### How docuconf adapts CLI11

CLI11 finds each raw value: a command-line flag wins over the environment, as in any CLI11 app (the platform only
sets the environment). docuconf then parses every value itself, because CLI11's conversions differ from the spec
in ways that matter:

- **Empty values:** CLI11 skips an empty environment variable. The spec says an empty string is a present value
  for a `string` and unset for every other type, so docuconf reads the process environment for that case.
- **Parsing:** CLI11 reads `010` as octal, `0x10` as hex, accepts `yes`/`on` for booleans and parses floats with
  the C locale functions. docuconf follows SPEC §5: base-10 64-bit integers (`out_of_range` beyond),
  `true`/`false` in any case, locale-independent floats with no NaN or infinity, values never trimmed.
- **Lists:** CLI11 splits on a `char` delimiter, drops empty items and treats `[a,b]` specially. docuconf splits
  `csv` lists on the declared separator and keeps every item, so the contract says `encoding: "csv"`.
- **Durations:** CLI11 has no duration type; docuconf reads Go syntax (`1m30s`), so the contract says
  `encoding: "go"`.
- **Errors:** CLI11 stops at the first bad value; docuconf reports every violation together.

CLI11 config files (`app.set_config`) are rejected at declaration time for now: values read from them would bypass
the contract. The declaration API has no profiles or config-file overlays (SPEC §4.4, §4.7), so a contract
exported from C++ never has them.

## Validate at boot

`DOCUCONF_PARSE(config, argc, argv)` works like `CLI11_PARSE`: it parses, validates every variable and file
input, binds the values, and returns from `main` with an exit code when it cannot continue (0 after `--help` or
`--docuconf-export`, 1 with every violation printed). The violations carry the spec's stable codes, and secret
values never appear:

```text
docuconf: 3 configuration problems:
  PORT: 0 is below min 1 (out_of_range)
  DATABASE_URL: value has scheme mysql, not one of postgres, postgresql (invalid_scheme)
  serving-tls: certificate expires at 2026-12-20T00:00:00Z, in 288h, less than minRemaining 720h (certificate_expiring)
```

Without the macro, `config.parse(argc, argv)` throws `CLI::ParseError` (as CLI11 does),
`docuconf::DeclarationError` or `docuconf::ValidationError` (with `violations()`, `has(code)` and
`codes_for(name)`). `config.load(env)` validates and binds a given environment map without CLI11, for tests.

The violations are also written to `/dev/termination-log` when it exists (or to `DOCUCONF_TERMINATION_LOG`),
so `kubectl describe pod` shows them. `DOCUCONF_FILE_ROOT` is prepended to every absolute file path, including
paths read from a `path_env` variable, for local development and tests. A secret that still holds an injector
reference (`vault:`, `op://`, `ref+`) is `invalid_type`, naming the scheme and never the value.

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
- **`reload: watch`:** not implemented. docuconf reads file inputs once, at boot, and rejects `watch` at
  declaration time.

## Export

The app itself exports its contract, without reading the environment:

```sh
./billing --docuconf-export contract.cue    # or --docuconf-export - for standard output
```

`config.export_cue()` returns the same text and `config.export_json()` its JSON form. The output starts with
`// Code generated by docuconf. DO NOT EDIT.`, a package clause named after the service, and
`import "docuconf.dev/contract"`; it is deterministic, with variables and file inputs sorted by name, and
`metadata.generator.language` is `cpp`.

`cpp` is not yet in the meta-schema's `generator.language` enum on docuconf-go `main`
([docuconf-go#7](https://github.com/docuconf/docuconf-go/pull/7) adds it), so `cue vet` of an exported contract
fails on that one field until it merges.

## Contract-first mode

Validate an environment against a contract given as JSON (`cue export contract.cue`), with no C++ declaration:

```cpp
auto contract = docuconf::Contract::from_json(contract_json_text);
docuconf::Values values = contract.load({{"PORT", "9090"}, {"TIMEOUT", "PT1.5S"}});
std::int64_t port = values.get("PORT")->as_int();
```

It parses every encoding in SPEC §5 (`csv`, `json` and `indexed` lists; `go`, `iso8601`, `seconds` and
`timespan` durations) and checks values with the same code as the declaration path. `load_process_env()` reads
the process environment and writes the termination log on failure. File inputs and overlays in the contract are
ignored; profiles are honoured.

## Conformance

The test suite runs the shared conformance cases (SPEC §12) through contract-first mode. It reads
`cases.json` from `DOCUCONF_CONFORMANCE`, falling back to `../docuconf-go/conformance/cases.json` next to this
repository, and fails instead of skipping when `DOCUCONF_REQUIRE_CONFORMANCE=1` and the file is missing:

```sh
DOCUCONF_CONFORMANCE=../docuconf-go/conformance/cases.json DOCUCONF_REQUIRE_CONFORMANCE=1 \
  ./build/tests/docuconf_tests --gtest_filter='Conformance.*'
```

No capability tags are skipped: integers are `std::int64_t` (`int64`), and `json` values are checked against
their JSON Schema (`json-schema`). Failures are reported by case id.

## Development

```sh
cmake -S . -B build -G Ninja
cmake --build build
DOCUCONF_SPEC_CUE=../docuconf-go/spec/cue ctest --test-dir build --output-on-failure
UPDATE_GOLDEN=1 ./build/tests/docuconf_tests --gtest_filter='Export.MatchesGolden'   # after an intended change
```

The export tests run `cue vet -c` on the golden contract against the meta-schema in `DOCUCONF_SPEC_CUE` (default
`../docuconf-go/spec/cue`) with the `cue` binary from `CUE`, `~/go/bin/cue` or `PATH`, and skip when either is
missing.

## Licence

MIT. See [LICENSE](LICENSE).
