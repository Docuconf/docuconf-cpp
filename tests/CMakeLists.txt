if(NOT TARGET GTest::gtest_main)
  find_package(GTest QUIET)
endif()
if(NOT TARGET GTest::gtest_main)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

if(DOCUCONF_FILE_INPUTS)
  add_executable(docuconf_tests
    conformance_test.cpp
    duration_test.cpp
    export_test.cpp
    files_test.cpp
    load_test.cpp)
  target_link_libraries(docuconf_tests PRIVATE docuconf::docuconf GTest::gtest_main OpenSSL::Crypto)
else()
  # The environment-only build: everything that does not need file inputs.
  add_executable(docuconf_tests
    conformance_test.cpp
    duration_test.cpp)
  target_link_libraries(docuconf_tests PRIVATE docuconf::docuconf GTest::gtest_main)
endif()
target_compile_definitions(docuconf_tests PRIVATE
  DOCUCONF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(docuconf_tests DISCOVERY_MODE PRE_TEST)
