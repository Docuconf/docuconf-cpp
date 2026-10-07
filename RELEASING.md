# Releasing docuconf-cpp

C++ has no central package registry, so a release is a git tag plus a GitHub release with a source tarball.
Consumers use `FetchContent` (pinned to the tag) or install the library and use `find_package(docuconf)`.

1. Update `VERSION` in the `project()` call in `CMakeLists.txt` (it is also the `metadata.generator.version` of
   exported contracts), then regenerate the golden file and the example contract:
   ```sh
   cmake --build build
   UPDATE_GOLDEN=1 ./build/tests/docuconf_tests --gtest_filter='Export.MatchesGolden'
   ./build/examples/orders/orders --docuconf-export examples/orders/contract.cue
   ```
2. Make sure CI is green on `main`, including conformance and `cue vet`.
3. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`.
4. `.github/workflows/release.yml` checks that the tag matches the CMake version, builds
   `docuconf-cpp-<version>.tar.gz` with `git archive`, and creates the GitHub release with the tarball and its
   SHA-256. It uses the workflow's own `GITHUB_TOKEN`; no secret is needed.

## GitHub Packages and Releases

GitHub Packages has no C++ registry, so the GitHub copy of each release is the GitHub Release. `release.yml` checks
the tag against the CMake version, builds `docuconf-cpp-<version>.tar.gz` with `git archive`, creates the GitHub
release if it does not exist (a re-run reuses it), and attaches the tarball and `docuconf-cpp-<version>.tar.gz.sha256`.
It needs no setup: it uses only the workflow's own `GITHUB_TOKEN` (`contents: write`), which the `Docuconf`
organization allows unless it has restricted workflow permissions under Organization settings > Actions.

### Installing from GitHub

No token is needed for a public repository. With CMake, fetch the released tarball and pin its hash:

```cmake
include(FetchContent)
FetchContent_Declare(docuconf
  URL https://github.com/docuconf/docuconf-cpp/releases/download/v0.1.0/docuconf-cpp-0.1.0.tar.gz
  URL_HASH SHA256=<the hash from docuconf-cpp-0.1.0.tar.gz.sha256>)
FetchContent_MakeAvailable(docuconf)
```

or download it, check it with `sha256sum -c docuconf-cpp-0.1.0.tar.gz.sha256`, and build and install it for
`find_package(docuconf)`.

## Later: vcpkg and Conan

Submitting to package managers is a separate step, done once a release exists:

- **vcpkg:** add a port to [microsoft/vcpkg](https://github.com/microsoft/vcpkg) (`ports/docuconf/portfile.cmake`
  with `vcpkg_from_github` on the release tag and its SHA-512, plus `vcpkg.json` depending on `cli11`,
  `nlohmann-json`, `re2`, `openssl`, `yaml-cpp`, `tomlplusplus` and `json-schema-validator`), then open a pull
  request there.
- **Conan:** add a recipe to [conan-center-index](https://github.com/conan-io/conan-center-index)
  (`recipes/docuconf/all/conanfile.py` and `conandata.yml` pointing at the release tarball and its SHA-256).

Both build with `-DDOCUCONF_FETCH_DEPS=OFF`, so every dependency comes from the package manager.

The project is MIT licensed (`LICENSE`).
