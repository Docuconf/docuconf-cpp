#!/usr/bin/env bash
# Builds a fresh consumer project from README.md exactly as a new user would:
# the first ```cmake block is its CMakeLists.txt and the first ```cpp block
# its main.cpp. With --env-only, the README's DOCUCONF_FILE_INPUTS line goes
# before FetchContent_MakeAvailable. docuconf itself comes from this checkout
# (FETCHCONTENT_SOURCE_DIR_DOCUCONF) instead of GIT_TAG main, so CI tests the
# change under review; every other dependency resolves as it would for the
# user, including fetching what the machine does not have.
#
#   tests/readme_consumer.sh WORK_DIR [--env-only] [extra cmake args...]
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$1"
shift
env_only=0
if [[ "${1:-}" == "--env-only" ]]; then
  env_only=1
  shift
fi
rm -rf "$work"
mkdir -p "$work"

python3 - "$repo/README.md" "$work" "$env_only" <<'PY'
import pathlib, re, sys
readme, work, env_only = pathlib.Path(sys.argv[1]).read_text(), pathlib.Path(sys.argv[2]), sys.argv[3] == "1"
cmake = re.findall(r"^```cmake\n(.*?)^```", readme, re.S | re.M)
cpp = re.findall(r"^```cpp\n(.*?)^```", readme, re.S | re.M)
lists = cmake[0]
if env_only:
    lists = lists.replace("FetchContent_MakeAvailable(docuconf)", cmake[1] + "FetchContent_MakeAvailable(docuconf)")
(work / "CMakeLists.txt").write_text(lists)
(work / "main.cpp").write_text(cpp[0])
PY

cd "$work"
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_DOCUCONF="$repo" "$@"
cmake --build build
bin=build/billing

out="$(env -i DATABASE_URL=postgres://billing:s3cret@localhost:5432/billing "$bin")"
echo "$out"
[[ "$out" == "billing: listening on :8080, timeout 30000ms" ]] || { echo "unexpected output"; exit 1; }

set +e
out="$(env -i PORT=0 DATABASE_URL=mysql://billing:s3cret@db/billing REQUEST_TIMEOUT=30 "$bin" 2>&1)"
code=$?
set -e
echo "$out"
expected='docuconf: 3 configuration problems:
  PORT: 0 is below min 1 (out_of_range)
  DATABASE_URL: value has scheme mysql, not one of postgres, postgresql (invalid_scheme)
  REQUEST_TIMEOUT: "30" is not a duration such as "1m30s" (invalid_type)'
[[ $code -eq 1 && "$out" == "$expected" ]] || { echo "unexpected error output (exit $code)"; exit 1; }
grep -qF "$expected" "$repo/README.md" || { echo "README.md does not show this output"; exit 1; }
"$bin" --docuconf-export - | head -3
echo "readme consumer: ok"
