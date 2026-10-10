# orders: a docuconf example

A tiny HTTP service ([cpp-httplib](https://github.com/yhirose/cpp-httplib)) whose configuration is declared
with docuconf next to a CLI11 app. It shows:

- seven environment variables with the metadata the contract needs (descriptions, secrets, ranges, an enum, a
  URL scheme, a list, a key set and a duration);
- `GET /healthz`, which returns `ok`, and `GET /config`, which returns the typed configuration as JSON with the
  secrets redacted, and `POST /webhooks/payments`, which checks a signature against a key set;
- the boot check: a bad environment stops the service with every problem listed;
- [`contract.cue`](contract.cue), exported by the app itself. `WORKER_COUNT` is documented with `.doc()`: the
  first paragraph of its doc comment is the description and the rest its `details`, which
  `docuconf docs contract.cue` renders into CONFIG.md and CONFIG.agents.md.

| Variable | Type | Rules |
|---|---|---|
| `PORT` | int | 1–65535, default 8080 |
| `LOG_LEVEL` | enum | `debug`, `info`, `warn`, `error`; default `info` |
| `DATABASE_URL` | url | secret, required, scheme `postgres`, at most 2048 characters |
| `ALLOWED_ORIGINS` | list of strings (comma-separated) | at least 1 item; default `http://localhost:3000` |
| `REQUEST_TIMEOUT` | duration (Go syntax, `30s`) | 1s–5m, default `30s` |
| `WORKER_COUNT` | int | 1–64, default 4 |
| `WEBHOOK_KEYS` | key set (comma-separated) | always secret, optional; 1–2 keys of 32–256 characters each |

They are read from the environment only, which is what the platform validates before deploy. `--help` lists
them in an `Environment variables` section. `PORT` also opts in to a command-line flag, `--port`, for local
runs; a value given that way wins over the environment, gets the same checks, and is named in errors
(`PORT (--port): 0 is below min 1`).

## Run it locally

From the repository root:

```sh
cmake -S . -B build -G Ninja && cmake --build build --target orders
DATABASE_URL=postgres://orders:secret@localhost:5432/orders PORT=8080 ./build/examples/orders/orders
curl localhost:8080/healthz
curl localhost:8080/config
```

The example also builds on its own (`cmake -S examples/orders -B build-orders`), against the SDK in this
repository.

## When the configuration is wrong

With `PORT=0` and no `DATABASE_URL`, the service does not start. This is the real output:

```text
$ PORT=0 ./build/examples/orders/orders
docuconf: 2 configuration problems:
  PORT: 0 is below min 1 (out_of_range)
  DATABASE_URL: is required but not set (missing_required)
$ echo $?
1
```

In Kubernetes the same lines go to `/dev/termination-log`, so `kubectl describe pod` shows them.

## Rotate a key

`WEBHOOK_KEYS` is a key set, a `docuconf::KeySet`: `POST /webhooks/payments` accepts a body whose `X-Signature`
header is the hex HMAC-SHA256 of the body under any key in the set, checked with `KeySet::verify`, which tries
every key ([`webhook.hpp`](webhook.hpp)). A variable is read once, at
start, so a new key reaches the service only when the pods restart; with two keys valid at once, no webhook is
turned away while that happens:

1. Add the new key as the second item (`old,new` in the Secret), and roll out.
2. Switch the sender to the new key.
3. Remove the old key (`new`), and roll out.

The generated [`CONFIG.md`](CONFIG.md#webhook_keys) prints these steps for every key set, so the declaration's
doc comment does not repeat them.

The contract allows 1 or 2 keys of 32 to 256 characters each, so a trailing comma or a truncated key stops the
service at boot instead of locking out the sender:

```text
$ DATABASE_URL=postgres://orders:pw@localhost:5432/orders WEBHOOK_KEYS=old-webhook-key-0123456789abcdef0123, \
    ./build/examples/orders/orders
docuconf: 1 configuration problem:
  WEBHOOK_KEYS: key 2 is empty (out_of_range)
```

In a values file, the key set is a `secretKeyRef`:

```yaml
WEBHOOK_KEYS: # a key set: one Secret key holding "old,new" while rotating
  secretKeyRef: {name: orders-webhooks, key: keys}
```

[`webhook_test.cpp`](webhook_test.cpp) walks through a rotation, and [`smoke.sh`](smoke.sh) posts webhooks signed
with both keys. [SPEC section 6.1](https://github.com/docuconf/docuconf-go/blob/main/spec/SPEC.md#61-rotation)
covers rotation in general.

## Export the contract

```sh
./build/examples/orders/orders --docuconf-export examples/orders/contract.cue
```

Export reads only the declaration, never the environment. CI re-exports the contract and fails if it differs
from the committed file, then vets it with `cue vet -c` against the meta-schema. [`smoke.sh`](smoke.sh) starts
the service with a valid and an invalid environment and checks both, and the webhook key set.

## Generated docs

[`CONFIG.md`](CONFIG.md), the reference for developers, and [`CONFIG.agents.md`](CONFIG.agents.md), the rules
and facts AI agents need, are generated from `contract.cue` by the `docuconf` CLI from
[docuconf-go](https://github.com/docuconf/docuconf-go), through the docs model in [`docs.json`](docs.json).
Never edit them by hand; regenerate them after exporting the contract (CI fails if they are out of date):

```sh
cd examples/orders
docuconf docs contract.cue -o CONFIG.md
docuconf docs contract.cue --format agents -o CONFIG.agents.md
docuconf docs contract.cue --format model -o docs.json
```

## Deploying

The platform validates what it intends to supply against `contract.cue` before anything is deployed: with
`docuconf vet` and `docuconf render` from [docuconf-go](https://github.com/docuconf/docuconf-go), or with the
Helm chart in [docuconf-go/helm](https://github.com/docuconf/docuconf-go/tree/main/helm). A missing
`DATABASE_URL` or `PORT: 0` is then rejected at composition time, and `DATABASE_URL` must come from a Secret
reference, never a literal. The service checks the same rules again at boot.
