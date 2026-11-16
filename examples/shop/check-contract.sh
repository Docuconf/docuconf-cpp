#!/usr/bin/env bash
# Checks the shop contract against the docuconf meta-schema: `cue vet -c`
# on contract.cue and on contract.json (the file the app loads), then checks
# that both describe the same contract, defaults included.
#
#   examples/shop/check-contract.sh path/to/docuconf-go/spec/cue
set -euo pipefail

spec="${1:?usage: $0 path/to/docuconf-go/spec/cue}"
cue="${CUE:-cue}"
here="$(cd "$(dirname "$0")" && pwd)"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

cp -r "$spec/cue.mod" "$spec/contract" "$dir/"
mkdir "$dir/cue" "$dir/json"
cp "$here/contract.cue" "$dir/cue/contract.cue"
# JSON is CUE: the same contract.#Contract around contract.json.
{
  printf 'package shop\n\nimport "docuconf.dev/contract"\n\ncontract.#Contract & '
  cat "$here/contract.json"
} >"$dir/json/contract.cue"

cd "$dir"
"$cue" vet -c ./cue
echo "contract.cue: ok"
"$cue" vet -c ./json
echo "contract.json: ok"
"$cue" export ./cue --out json >cue.json
"$cue" export ./json --out json >json.json
diff -u --label contract.cue cue.json --label contract.json json.json
echo "contract.cue and contract.json agree"
