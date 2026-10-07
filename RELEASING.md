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
2. In `README.md`, change the install block from `GIT_TAG main` to `GIT_TAG v<version>` and drop the
   "no release tag yet" note. Until the first release the README must keep `GIT_TAG main`, because no tag
   exists. Make sure CI is green on `main`, including conformance and `cue vet`.
3. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`.
4. `.github/workflows/release.yml` checks that the tag matches the CMake version, builds
   `docuconf-cpp-<version>.tar.gz` with `git archive`, and creates the GitHub release with the tarball and its
   SHA-256. It uses the workflow's own `GITHUB_TOKEN`; no secret is needed.

## Later: vcpkg and Conan

Submitting to package managers is a separate step, done once a release exists:

- **vcpkg:** add a port to [microsoft/vcpkg](https://github.com/microsoft/vcpkg) (`ports/docuconf/portfile.cmake`
  with `vcpkg_from_github` on the release tag and its SHA-512, plus `vcpkg.json` depending on `cli11`,
  `nlohmann-json`, `re2`, `openssl`, `yaml-cpp`, `tomlplusplus` and `json-schema-validator`, with a
  `files` feature mapping to `DOCUCONF_FILE_INPUTS`), then open a pull request there.
- **Conan:** add a recipe to [conan-center-index](https://github.com/conan-io/conan-center-index)
  (`recipes/docuconf/all/conanfile.py` and `conandata.yml` pointing at the release tarball and its SHA-256).

Both build with `-DDOCUCONF_FETCH_DEPS=OFF`, so every dependency comes from the package manager. That is also
what the install rules need: a build that fetched CLI11, nlohmann/json, RE2, yaml-cpp or toml++ generates no
install rules (only a fetched json-schema-validator is installed alongside docuconf), because the installed
`docuconfConfig.cmake` finds its dependencies with `find_package`.

The project is MIT licensed (`LICENSE`).
