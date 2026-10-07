#!/usr/bin/env bash
# Smoke test for the orders example: starts it with a valid environment and
# checks its endpoints, then starts it with a broken one and checks that it
# refuses to boot with every violation listed.
#
#   examples/orders/smoke.sh [path/to/orders]
set -euo pipefail

bin="${1:-build/examples/orders/orders}"
port="${SMOKE_PORT:-18087}"
secret="postgres://orders:smoke-s3cret@db.internal:5432/orders"
log="$(mktemp)"
trap 'rm -f "$log"; [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true' EXIT

echo "== valid environment"
env -i PATH="$PATH" PORT="$port" DATABASE_URL="$secret" ALLOWED_ORIGINS="https://shop.example.com" \
  "$bin" >"$log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do
  curl -fsS "http://127.0.0.1:$port/healthz" >/dev/null 2>&1 && break
  sleep 0.1
done
health="$(curl -fsS "http://127.0.0.1:$port/healthz")"
[[ "$health" == "ok" ]] || { echo "GET /healthz returned '$health'"; cat "$log"; exit 1; }
echo "GET /healthz: $health"
config="$(curl -fsS "http://127.0.0.1:$port/config")"
echo "GET /config: $config"
if [[ "$config" == *"smoke-s3cret"* ]]; then echo "GET /config leaks the secret"; exit 1; fi
[[ "$config" == *'"DATABASE_URL":"***"'* ]] || { echo "GET /config does not redact DATABASE_URL"; exit 1; }
kill "$pid"
wait "$pid" 2>/dev/null || true
pid=""

echo "== PORT=0 and no DATABASE_URL"
set +e
out="$(env -i PATH="$PATH" PORT=0 "$bin" 2>&1)"
code=$?
set -e
echo "$out"
[[ $code -ne 0 ]] || { echo "expected a non-zero exit"; exit 1; }
[[ "$out" == *missing_required* ]] || { echo "expected missing_required"; exit 1; }
[[ "$out" == *out_of_range* ]] || { echo "expected out_of_range"; exit 1; }
echo "exit code: $code"

echo "== --port 0 (the opt-in flag) is checked like the environment"
set +e
out="$(env -i PATH="$PATH" DATABASE_URL="$secret" "$bin" --port 0 2>&1)"
code=$?
set -e
echo "$out"
[[ $code -eq 1 ]] || { echo "expected exit code 1, got $code"; exit 1; }
[[ "$out" == *"PORT (--port): 0 is below min 1 (out_of_range)"* ]] || { echo "expected the flag to be named"; exit 1; }
[[ "$out" != *"smoke-s3cret"* ]] || { echo "the secret leaked"; exit 1; }

echo "== --help lists the environment"
help="$(env -i PATH="$PATH" "$bin" --help)"
[[ "$help" == *"DATABASE_URL"*"REQUIRED, secret"* ]] || { echo "--help does not list DATABASE_URL"; echo "$help"; exit 1; }
[[ "$help" != *"--database-url"* ]] || { echo "a secret became a flag"; exit 1; }
echo "smoke: ok"
