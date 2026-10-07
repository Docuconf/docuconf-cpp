cmake_minimum_required(VERSION 3.16)

project(docuconf
  # x-release-please-start-version
  VERSION 0.1.0
  # x-release-please-end
  DESCRIPTION "Typed configuration contracts for CLI11"
  HOMEPAGE_URL "https://github.com/docuconf/docuconf-cpp"
  LANGUAGES CXX)

set(DOCUCONF_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(DOCUCONF_IS_TOP_LEVEL ON)
endif()

option(DOCUCONF_BUILD_TESTS "Build the docuconf tests" ${DOCUCONF_IS_TOP_LEVEL})
option(DOCUCONF_BUILD_EXAMPLES "Build the examples" ${DOCUCONF_IS_TOP_LEVEL})
option(DOCUCONF_INSTALL "Generate the install target" ${DOCUCONF_IS_TOP_LEVEL})
option(DOCUCONF_FETCH_DEPS "Fetch dependencies that find_package cannot find" ON)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(GNUInstallDirs)

# Dependencies. Every one is found with find_package first (system packages,
# vcpkg, Conan); the ones Ubuntu does not package are fetched at a pinned tag.
find_package(CLI11 2.3 CONFIG REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(tomlplusplus 3 REQUIRED)

find_package(re2 CONFIG QUIET)
if(NOT TARGET re2::re2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(RE2 REQUIRED IMPORTED_TARGET GLOBAL re2)
  add_library(re2::re2 ALIAS PkgConfig::RE2)
  set(DOCUCONF_RE2_FROM_PKGCONFIG ON)
endif()

find_package(nlohmann_json_schema_validator CONFIG QUIET)
if(NOT TARGET nlohmann_json_schema_validator::validator AND NOT TARGET nlohmann_json_schema_validator)
  if(NOT DOCUCONF_FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json_schema_validator not found and DOCUCONF_FETCH_DEPS is OFF")
  endif()
  set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_INSTALL ${DOCUCONF_INSTALL} CACHE BOOL "" FORCE)
  FetchContent_Declare(json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG c780404a84dd9ba978ba26bc58d17cb43fa7bc80 # 2.4.0
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(json_schema_validator)
  set_target_properties(nlohmann_json_schema_validator PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(TARGET nlohmann_json_schema_validator::validator)
  set(DOCUCONF_JSV_TARGET nlohmann_json_schema_validator::validator)
else()
  set(DOCUCONF_JSV_TARGET nlohmann_json_schema_validator)
endif()

add_library(docuconf
  src/contract.cpp
  src/declaration.cpp
  src/duration.cpp
  src/errors.cpp
  src/export.cpp
  src/files.cpp
  src/schema.cpp
  src/tls.cpp
  src/value.cpp)
add_library(docuconf::docuconf ALIAS docuconf)
target_compile_features(docuconf PUBLIC cxx_std_17)
target_include_directories(docuconf PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(docuconf
  PUBLIC CLI11::CLI11 nlohmann_json::nlohmann_json
  PRIVATE re2::re2 OpenSSL::SSL OpenSSL::Crypto yaml-cpp::yaml-cpp tomlplusplus::tomlplusplus
          ${DOCUCONF_JSV_TARGET})
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
