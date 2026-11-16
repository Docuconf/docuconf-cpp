#!/usr/bin/env bash
# Smoke test for the shop example: runs the real binary as each role with a
# valid environment, then with LOG_FORMAT=json and no LOG_SCHEMA_URL, and
# checks stdout, stderr and the exit code (README cases 1, 2 and 6).
#
#   examples/shop/smoke.sh [path/to/shop]
set -euo pipefail

bin="${1:-build/examples/shop/shop}"
secret="postgres://shop:smoke-s3cret@db:5432/shop"
root="$(mktemp -d)"  # an empty file root: no signing key
trap 'rm -rf "$root"' EXIT

run() { env -i PATH="$PATH" DATABASE_URL="$secret" DOCUCONF_FILE_ROOT="$root" "$@" "$bin"; }

expect_ok() {
  local want="$1"; shift
  local out code
  set +e
  out="$(run "$@" 2>&1)"
  code=$?
  set -e
  echo "$out"
  [[ $code -eq 0 ]] || { echo "expected exit code 0, got $code"; exit 1; }
  [[ "$out" == "$want" ]] || { echo "expected '$want'"; exit 1; }
}

echo "== 1: web"
expect_ok "shop web: port 8080, origins https://shop.example" DOCUCONF_ROLE=web ALLOWED_ORIGINS=https://shop.example

echo "== 2: worker, without ALLOWED_ORIGINS"
expect_ok "shop worker: queues default,mail, webhooks off" DOCUCONF_ROLE=worker QUEUES=default,mail

echo "== 6: LOG_FORMAT=json without LOG_SCHEMA_URL"
set +e
out="$(run DOCUCONF_ROLE=worker QUEUES=default LOG_FORMAT=json 2>/dev/null)"
err="$(run DOCUCONF_ROLE=worker QUEUES=default LOG_FORMAT=json 2>&1 >/dev/null)"
code=$?
set -e
echo "$err"
[[ $code -eq 1 ]] || { echo "expected exit code 1, got $code"; exit 1; }
[[ -z "$out" ]] || { echo "expected nothing on stdout, got '$out'"; exit 1; }
[[ "$err" == *'LOG_SCHEMA_URL: is required when LOG_FORMAT is "json" (missing_required)'* ]] ||
  { echo "expected LOG_SCHEMA_URL missing_required"; exit 1; }
[[ "$err" != *smoke-s3cret* ]] || { echo "the secret leaked"; exit 1; }
echo "smoke: ok"
