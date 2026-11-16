# shop: a docuconf example

One program that runs as two roles, `web` and `worker`, configured **contract-first**: the contract is
[`contract.json`](contract.json), written by hand, and the app validates its environment against it with
`docuconf::Contract` at startup. There is no real server, database or queue: with a valid configuration the
app prints one line and exits 0. It shows:

- **roles** ([SPEC §4.9](https://github.com/docuconf/docuconf-go/blob/main/spec/SPEC.md)): `DOCUCONF_ROLE`
  names the workload's role, and an input marked with roles is required only in those. The web role needs
  `ALLOWED_ORIGINS`, the worker role `QUEUES`;
- **requiredIf** (SPEC §4.2.1): `LOG_SCHEMA_URL` is required when `LOG_FORMAT` is `json`, and the
  `webhook-signing-key` file when `WEBHOOKS_ENABLED` is `true`;
- an **included fragment** (SPEC §4.8): `includes: ["otel@1"]`, with the OpenTelemetry variables inline. They
  are checked at boot like any other variable.

| Input | Type | Rules | Roles |
|---|---|---|---|
| `DOCUCONF_ROLE` | enum | required; `web`, `worker` | all |
| `DATABASE_URL` | url | required, secret; scheme `postgres` or `postgresql` | all |
| `PORT` | int | 1–65535, default 8080 | `web` |
| `ALLOWED_ORIGINS` | list of strings (comma-separated) | required | `web` |
| `QUEUES` | list of strings (comma-separated) | required | `worker` |
| `WEBHOOKS_ENABLED` | bool | default `false` | `worker` |
| `LOG_FORMAT` | enum | `text`, `json`; default `text` | all |
| `LOG_SCHEMA_URL` | url | scheme `https`; required when `LOG_FORMAT` is `json` | all |
| `OTEL_*` | from `otel@1` | optional; `OTEL_EXPORTER_OTLP_PROTOCOL` is `grpc`, `http/protobuf` or `http/json` | all |
| file `webhook-signing-key` | text at `/etc/shop/webhooks/signing.key` | secret, at least 32 characters; required when `WEBHOOKS_ENABLED` is `true` | `worker` |

The files:

- [`contract.json`](contract.json): the contract, compiled into the binary (CMake embeds it) and read with
  `docuconf::Contract::from_json`. [`contract.cue`](contract.cue) is the same contract in CUE;
  [`check-contract.sh`](check-contract.sh) vets both against the meta-schema and checks that they agree.
- [`shop.hpp`](shop.hpp), [`shop.cpp`](shop.cpp): the typed `shop::Config` read from the loaded values, the
  printed line, and `shop::boot`, which prints it or every problem. [`main.cpp`](main.cpp) calls `boot` with
  `contract().load_process_env()`.
- [`shop_test.cpp`](shop_test.cpp): the test matrix below. [`smoke.sh`](smoke.sh): cases 1, 2 and 6 against
  the real binary.

## Run it

From the repository root:

```sh
cmake -S . -B build -G Ninja && cmake --build build --target shop shop_test
export DATABASE_URL=postgres://shop@db:5432/shop
DOCUCONF_ROLE=web ALLOWED_ORIGINS=https://shop.example ./build/examples/shop/shop
DOCUCONF_ROLE=worker QUEUES=default,mail ./build/examples/shop/shop
```

```text
shop web: port 8080, origins https://shop.example
shop worker: queues default,mail, webhooks off
```

With `WEBHOOKS_ENABLED=true`, the worker reads its signing key from `/etc/shop/webhooks/signing.key`, under
`DOCUCONF_FILE_ROOT` when that is set, and prints `webhooks on`. The worker's queues are printed from the typed
configuration, not from the raw environment.

The example also builds on its own (`cmake -S examples/shop -B build-shop`), against the SDK in this repository.

## When the configuration is wrong

The app does not start: it lists every problem on standard error, writes them to the termination log, and exits
1. A secret is never printed. This is the real output:

```text
$ DOCUCONF_ROLE=worker QUEUES=default LOG_FORMAT=json WEBHOOKS_ENABLED=true ./build/examples/shop/shop
docuconf: 2 configuration problems:
  LOG_SCHEMA_URL: is required when LOG_FORMAT is "json" (missing_required)
  webhook-signing-key: /etc/shop/webhooks/signing.key does not exist, and is required when WEBHOOKS_ENABLED is true (file_missing)
$ echo $?
1
```

## Test matrix

[`shop_test.cpp`](shop_test.cpp) runs each case with `DATABASE_URL=postgres://shop@db:5432/shop` and a fresh,
empty `DOCUCONF_FILE_ROOT`. A failing case must report exactly the violations listed.

| # | Environment (plus `DATABASE_URL`) | Expected |
|---|---|---|
| 1 | `DOCUCONF_ROLE=web ALLOWED_ORIGINS=https://shop.example` | ok: `shop web: port 8080, origins https://shop.example` |
| 2 | `DOCUCONF_ROLE=worker QUEUES=default,mail` | ok: the worker needs no `ALLOWED_ORIGINS` |
| 3 | `DOCUCONF_ROLE=web` | `ALLOWED_ORIGINS` `missing_required` |
| 4 | `QUEUES=default` (no role) | `DOCUCONF_ROLE` `missing_required`, and nothing else |
| 5 | `DOCUCONF_ROLE=cron` | `DOCUCONF_ROLE` `not_in_enum`, and nothing else |
| 6 | `DOCUCONF_ROLE=worker QUEUES=default LOG_FORMAT=json` | `LOG_SCHEMA_URL` `missing_required`: `is required when LOG_FORMAT is "json"` |
| 7 | as 6, plus `LOG_SCHEMA_URL=https://logs.example/v1.json` | ok |
| 8 | `DOCUCONF_ROLE=worker QUEUES=default WEBHOOKS_ENABLED=true`, no key file | `webhook-signing-key` `file_missing`: `, and is required when WEBHOOKS_ENABLED is true` |
| 9 | as 8, with a key of 32 or more characters at the path under the root | ok: `webhooks on` |
| 10 | `DOCUCONF_ROLE=web ALLOWED_ORIGINS=https://shop.example WEBHOOKS_ENABLED=true` | ok: the key file belongs to the worker |
| 11 | `DOCUCONF_ROLE=web ALLOWED_ORIGINS=https://shop.example OTEL_EXPORTER_OTLP_PROTOCOL=carrier-pigeon` | `OTEL_EXPORTER_OTLP_PROTOCOL` `not_in_enum`: fragment variables are checked at boot |

```sh
./build/examples/shop/shop_test
examples/shop/smoke.sh build/examples/shop/shop
```

## Generated docs

[`CONFIG.md`](CONFIG.md) and [`CONFIG.agents.md`](CONFIG.agents.md) are generated from `contract.cue` by the
`docuconf` CLI from [docuconf-go](https://github.com/docuconf/docuconf-go), through the docs model in
[`docs.json`](docs.json). Never edit them by hand; regenerate them after changing the contract (CI fails if they
are out of date):

```sh
cd examples/shop
docuconf docs contract.cue -o CONFIG.md
docuconf docs contract.cue --format agents -o CONFIG.agents.md
docuconf docs contract.cue --format model -o docs.json
```
