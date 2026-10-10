#!/usr/bin/env bash
# Smoke test for the orders example: starts it with a valid environment and
# checks its endpoints, then starts it with a broken one and checks that it
# refuses to boot with every violation listed, and posts webhooks signed
# with each key of a key set that is mid-rotation. Needs curl and openssl.
#
#   examples/orders/smoke.sh [path/to/orders]
set -euo pipefail

bin="${1:-build/examples/orders/orders}"
port="${SMOKE_PORT:-18087}"
secret="postgres://orders:smoke-s3cret@db.internal:5432/orders"
# Two webhook keys: the old one and, mid-rotation, the new one.
old_key='old-webhook-key-0123456789abcdef0123'
new_key='new-webhook-key-0123456789abcdef0123'
log="$(mktemp)"
trap 'rm -f "$log"; [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true' EXIT

echo "== valid environment"
env -i PATH="$PATH" PORT="$port" DATABASE_URL="$secret" ALLOWED_ORIGINS="https://shop.example.com" \
  WEBHOOK_KEYS="$old_key,$new_key" "$bin" >"$log" 2>&1 &
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
if [[ "$config" == *"smoke-s3cret"* || "$config" == *"webhook-key"* ]]; then echo "GET /config leaks a secret"; exit 1; fi
if grep -q -e smoke-s3cret -e webhook-key "$log"; then echo "the log leaks a secret"; exit 1; fi
[[ "$config" == *'"DATABASE_URL":"***"'* ]] || { echo "GET /config does not redact DATABASE_URL"; exit 1; }
[[ "$config" == *'"WEBHOOK_KEYS":"***"'* ]] || { echo "GET /config does not redact WEBHOOK_KEYS"; exit 1; }

echo "== webhooks mid-rotation: the old and the new key are accepted, any other is not"
code="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'X-Signature: 00' -d '{}' "http://127.0.0.1:$port/webhooks/payments")"
[[ "$code" == 401 ]] || { echo "an unsigned webhook got $code, want 401"; exit 1; }
body='{"order":"42","status":"paid"}'
for key in "$old_key" "$new_key" "other-webhook-key-0123456789abcdef"; do
  sig="$(printf '%s' "$body" | openssl dgst -sha256 -hmac "$key" | sed 's/.*= //')"
  code="$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Signature: $sig" -d "$body" \
    "http://127.0.0.1:$port/webhooks/payments")"
  want=204; [[ "$key" == other* ]] && want=401
  [[ "$code" == "$want" ]] || { echo "webhook signed with the ${key%%-*} key: got $code, want $want"; exit 1; }
done
echo "old and new key accepted, other key rejected"
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

echo "== an empty second webhook key (a trailing comma) fails at boot, without printing a key"
set +e
out="$(env -i PATH="$PATH" DATABASE_URL="$secret" WEBHOOK_KEYS="$old_key," "$bin" 2>&1)"
code=$?
set -e
echo "$out"
[[ $code -eq 1 ]] || { echo "expected exit code 1, got $code"; exit 1; }
[[ "$out" == *"WEBHOOK_KEYS: key 2 is empty (out_of_range)"* ]] ||
  { echo "expected WEBHOOK_KEYS out_of_range for the empty key"; exit 1; }
[[ "$out" != *"webhook-key"* ]] || { echo "a webhook key leaked"; exit 1; }

echo "== --help lists the environment"
help="$(env -i PATH="$PATH" "$bin" --help)"
[[ "$help" == *"DATABASE_URL"*"REQUIRED, secret"* ]] || { echo "--help does not list DATABASE_URL"; echo "$help"; exit 1; }
[[ "$help" != *"--database-url"* ]] || { echo "a secret became a flag"; exit 1; }
echo "smoke: ok"
