cmake_minimum_required(VERSION 3.16)

project(docuconf
  # x-release-please-start-version
  VERSION 0.1.0
  # x-release-please-end
  DESCRIPTION "Typed configuration contracts for CLI11"
  HOMEPAGE_URL "https://docuconf.dev/languages/cpp/"
  LANGUAGES CXX)

set(DOCUCONF_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(DOCUCONF_IS_TOP_LEVEL ON)
endif()

option(DOCUCONF_BUILD_TESTS "Build the docuconf tests" ${DOCUCONF_IS_TOP_LEVEL})
option(DOCUCONF_BUILD_EXAMPLES "Build the examples" ${DOCUCONF_IS_TOP_LEVEL})
# Whether DOCUCONF_INSTALL was set by the user or took its default.
if(NOT DEFINED DOCUCONF_INSTALL)
  set(DOCUCONF_INSTALL_DEFAULTED ON CACHE INTERNAL "")
endif()
option(DOCUCONF_INSTALL
  "Generate the install target (needs CLI11, nlohmann_json, RE2, yaml-cpp and toml++ from find_package)"
  ${DOCUCONF_IS_TOP_LEVEL})
option(DOCUCONF_FETCH_DEPS "Fetch dependencies that are neither targets already nor found by find_package" ON)
option(DOCUCONF_FILE_INPUTS
  "File inputs (TLS key pairs, CA bundles, keystores, config files). OFF gives an environment-only build without OpenSSL, yaml-cpp and toml++"
  ON)

# A top-level build without a build type would install the fetched
# json-schema-validator without per-configuration targets, which breaks
# find_package(docuconf) later; default to Release.
if(DOCUCONF_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(GNUInstallDirs)

# Dependencies. Each one is taken, in order, from a target the parent
# project already defines (for example its own FetchContent of CLI11), from
# find_package (system packages, vcpkg, Conan), or fetched at a pinned tag.
# Only OpenSSL, for file inputs, must be installed on the machine.
macro(docuconf_fetch name what)
  if(NOT DOCUCONF_FETCH_DEPS)
    message(FATAL_ERROR "docuconf: ${what} was not found and DOCUCONF_FETCH_DEPS is OFF. Install it or turn DOCUCONF_FETCH_DEPS on.")
  endif()
  message(STATUS "docuconf: fetching ${what}")
  list(APPEND DOCUCONF_FETCHED "${what}")
  FetchContent_Declare(${name} ${ARGN})
  FetchContent_MakeAvailable(${name})
endmacro()

if(NOT TARGET CLI11::CLI11)
  find_package(CLI11 2.3 CONFIG QUIET)
  if(NOT TARGET CLI11::CLI11)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
    docuconf_fetch(cli11 "CLI11"
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.4.2
      GIT_SHALLOW TRUE)
  endif()
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.10 QUIET)
  if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    docuconf_fetch(nlohmann_json "nlohmann_json"
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
      GIT_SHALLOW TRUE)
  endif()
endif()

# RE2 checks the `pattern` constraint.
if(NOT TARGET re2::re2)
  find_package(re2 CONFIG QUIET)
endif()
if(NOT TARGET re2::re2)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(RE2 QUIET IMPORTED_TARGET GLOBAL re2)
  endif()
  if(TARGET PkgConfig::RE2)
    add_library(re2::re2 ALIAS PkgConfig::RE2)
    set(DOCUCONF_RE2_FROM_PKGCONFIG ON)
  else()
    set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    # The last release that does not need Abseil.
    docuconf_fetch(re2 "RE2"
      GIT_REPOSITORY https://github.com/google/re2.git
      GIT_TAG 2023-03-01
      GIT_SHALLOW TRUE)
    set_target_properties(re2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
endif()

# json-schema-validator checks `json` variables and config files.
if(NOT TARGET nlohmann_json_schema_validator::validator AND NOT TARGET nlohmann_json_schema_validator)
  find_package(nlohmann_json_schema_validator CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json_schema_validator::validator AND NOT TARGET nlohmann_json_schema_validator)
  set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  # Installed only when docuconf will be (see the install rules below).
  if(DOCUCONF_INSTALL AND NOT DOCUCONF_FETCHED)
    set(JSON_VALIDATOR_INSTALL ON CACHE BOOL "" FORCE)
  else()
    set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
  endif()
  docuconf_fetch(json_schema_validator "nlohmann_json_schema_validator"
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG c780404a84dd9ba978ba26bc58d17cb43fa7bc80 # 2.4.0
    GIT_SHALLOW FALSE)
  set_target_properties(nlohmann_json_schema_validator PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(TARGET nlohmann_json_schema_validator::validator)
  set(DOCUCONF_JSV_TARGET nlohmann_json_schema_validator::validator)
else()
  set(DOCUCONF_JSV_TARGET nlohmann_json_schema_validator)
endif()

set(DOCUCONF_FILE_DEPS "")
if(DOCUCONF_FILE_INPUTS)
  find_package(OpenSSL 3.0 QUIET)
  if(NOT OPENSSL_FOUND)
    message(FATAL_ERROR
      "docuconf: file inputs (TLS key pairs, CA bundles, keystores) need OpenSSL 3 development files: "
      "apt-get install libssl-dev, dnf install openssl-devel, apk add openssl-dev or brew install openssl@3 "
      "(then -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)). "
      "For a service that only reads environment variables, set DOCUCONF_FILE_INPUTS to OFF instead: "
      "set(DOCUCONF_FILE_INPUTS OFF CACHE BOOL \"\") before FetchContent_MakeAvailable(docuconf).")
  endif()
  if(NOT TARGET yaml-cpp::yaml-cpp)
    find_package(yaml-cpp QUIET)
    if(NOT TARGET yaml-cpp::yaml-cpp AND TARGET yaml-cpp)
      add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
  endif()
  if(NOT TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    docuconf_fetch(yaml_cpp "yaml-cpp"
      GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
      GIT_TAG 0.8.0
      GIT_SHALLOW TRUE)
    set_target_properties(yaml-cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
  if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus 3 QUIET)
  endif()
  if(NOT TARGET tomlplusplus::tomlplusplus)
    docuconf_fetch(tomlplusplus "toml++"
      GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
      GIT_TAG v3.4.0
      GIT_SHALLOW TRUE)
  endif()
  set(DOCUCONF_FILE_DEPS OpenSSL::SSL OpenSSL::Crypto yaml-cpp::yaml-cpp tomlplusplus::tomlplusplus)
endif()

add_library(docuconf
  src/contract.cpp
  src/declaration.cpp
  src/doc.cpp
  src/duration.cpp
  src/errors.cpp
  src/export.cpp
  src/files.cpp
  src/schema.cpp
  src/value.cpp)
if(DOCUCONF_FILE_INPUTS)
  target_sources(docuconf PRIVATE src/tls.cpp)
  target_compile_definitions(docuconf PUBLIC DOCUCONF_FILE_INPUTS=1)
else()
  target_compile_definitions(docuconf PUBLIC DOCUCONF_FILE_INPUTS=0)
endif()
add_library(docuconf::docuconf ALIAS docuconf)
target_compile_features(docuconf PUBLIC cxx_std_17)
target_include_directories(docuconf PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(docuconf
  PUBLIC CLI11::CLI11 nlohmann_json::nlohmann_json
  PRIVATE re2::re2 ${DOCUCONF_JSV_TARGET} ${DOCUCONF_FILE_DEPS})
target_compile_definitions(docuconf PRIVATE DOCUCONF_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(docuconf PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(docuconf PROPERTIES VERSION ${PROJECT_VERSION} EXPORT_NAME docuconf)

if(DOCUCONF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DOCUCONF_BUILD_EXAMPLES)
  add_subdirectory(examples/orders)
endif()

# The installed package config finds its dependencies with find_package, so
# installing needs them to come from find_package too. The one exception is
# json-schema-validator, which few systems package: when docuconf fetches it,
# it is installed into the same prefix with its own package config. A build
# that fetched anything else (a bare machine, an env-only FetchContent) skips
# the install rules; consume docuconf with FetchContent there instead.
set(DOCUCONF_NOT_INSTALLABLE ${DOCUCONF_FETCHED})
list(REMOVE_ITEM DOCUCONF_NOT_INSTALLABLE "nlohmann_json_schema_validator")
if(DOCUCONF_INSTALL AND DOCUCONF_NOT_INSTALLABLE)
  string(REPLACE ";" ", " fetched "${DOCUCONF_NOT_INSTALLABLE}")
  set(why "docuconf: no install rules, because these dependencies were fetched rather than found with find_package: ${fetched}. Install them as packages to use cmake --install and find_package(docuconf), or add docuconf to your project with FetchContent.")
  if(NOT DOCUCONF_INSTALL_DEFAULTED)
    message(FATAL_ERROR "${why} (DOCUCONF_INSTALL is ON)")
  endif()
  message(STATUS "${why}")
  set(DOCUCONF_INSTALL OFF)
endif()

if(DOCUCONF_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS docuconf EXPORT docuconfTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/docuconf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT docuconfTargets
    NAMESPACE docuconf::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/docuconf)
  configure_package_config_file(cmake/docuconfConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/docuconfConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/docuconf)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/docuconfConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/docuconfConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/docuconfConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/docuconf)
endif()
