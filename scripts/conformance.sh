#!/usr/bin/env bash
# Runs the shared conformance suite (docuconf-go conformance/cases.json),
# failing if any case is skipped, the shared export check (the fixture in
# conformance/export, compared with golden.cue by `docuconf conformance
# export`) and the test that `cue vet`s the exported contract against the
# meta-schema (docuconf-go spec/cue). Not the full suite. Used by this repo's
# CI and by docuconf-go's downstream gate.
#
#   DOCUCONF_GO_DIR=/path/to/docuconf-go scripts/conformance.sh
#
# Needs a C++20 compiler, CMake, the libraries CI installs with apt (CLI11,
# nlohmann-json, RE2, OpenSSL, yaml-cpp, toml++, GoogleTest), cue on PATH (or
# CUE), and the docuconf CLI (DOCUCONF_CLI, else docuconf on PATH, else built
# from DOCUCONF_GO_DIR with go). Builds into build-conformance
# (DOCUCONF_BUILD_DIR overrides).
set -euo pipefail

: "${DOCUCONF_GO_DIR:?set DOCUCONF_GO_DIR to a docuconf-go checkout}"
DOCUCONF_GO_DIR=$(cd "$DOCUCONF_GO_DIR" && pwd)
export DOCUCONF_GO_DIR
export DOCUCONF_CONFORMANCE="${DOCUCONF_CONFORMANCE:-$DOCUCONF_GO_DIR/conformance/cases.json}"
export DOCUCONF_SPEC_CUE="${DOCUCONF_SPEC_CUE:-$DOCUCONF_GO_DIR/spec/cue}"
export DOCUCONF_REQUIRE_CONFORMANCE=1
export DOCUCONF_REQUIRE_NO_SKIPS=1
export DOCUCONF_REQUIRE_VET=1

# The vet test skips itself when cue is missing; here that is an error.
CUE=${CUE:-$(command -v cue || true)}
if [ -z "$CUE" ] || [ ! -x "$CUE" ]; then
  echo "cue not found: put it on PATH or set CUE" >&2
  exit 1
fi
export CUE

cd "$(dirname "$0")/.."
build=${DOCUCONF_BUILD_DIR:-build-conformance}

# The CLI that compares the export fixture with the golden contract: the
# one DOCUCONF_CLI names, else one built from DOCUCONF_GO_DIR, so it matches
# the suite, else docuconf on PATH.
if [ -z "${DOCUCONF_CLI:-}" ]; then
  if command -v go >/dev/null; then
    mkdir -p "$build"
    DOCUCONF_CLI="$(cd "$build" && pwd)/docuconf"
    (cd "$DOCUCONF_GO_DIR/cmd/docuconf" && go build -o "$DOCUCONF_CLI" .)
  else
    DOCUCONF_CLI=$(command -v docuconf || true)
    [ -n "$DOCUCONF_CLI" ] || { echo "docuconf CLI not found: set DOCUCONF_CLI or install go" >&2; exit 1; }
  fi
fi
export DOCUCONF_CLI
generator=()
if command -v ninja >/dev/null; then generator=(-G Ninja); fi
cmake -S . -B "$build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release -DDOCUCONF_BUILD_EXAMPLES=OFF
cmake --build "$build" --target docuconf_tests
ctest --test-dir "$build" --output-on-failure --no-tests=error \
  -R '^(Conformance\.|ExportFixture\.|Export\.VetsAgainstTheMetaSchema$)'
