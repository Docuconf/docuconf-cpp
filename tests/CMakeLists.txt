find_package(GTest REQUIRED)

add_executable(docuconf_tests
  conformance_test.cpp
  duration_test.cpp
  export_test.cpp
  files_test.cpp
  load_test.cpp)
target_link_libraries(docuconf_tests PRIVATE docuconf::docuconf GTest::gtest_main OpenSSL::Crypto)
target_compile_definitions(docuconf_tests PRIVATE
  DOCUCONF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(docuconf_tests DISCOVERY_MODE PRE_TEST)
