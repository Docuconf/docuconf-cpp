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
    devx_test.cpp
    doc_test.cpp
    duration_test.cpp
    export_test.cpp
    files_test.cpp
    load_test.cpp)
  target_link_libraries(docuconf_tests PRIVATE docuconf::docuconf GTest::gtest_main OpenSSL::Crypto)
else()
  # The environment-only build: everything that does not need file inputs.
  add_executable(docuconf_tests
    conformance_test.cpp
    devx_test.cpp
    doc_test.cpp
    duration_test.cpp)
  target_link_libraries(docuconf_tests PRIVATE docuconf::docuconf GTest::gtest_main)
endif()
target_compile_definitions(docuconf_tests PRIVATE
  DOCUCONF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(docuconf_tests DISCOVERY_MODE PRE_TEST)

# Declaration mistakes the compiler catches: each file must fail to build
# with the given message (the first line of the file).
file(GLOB DOCUCONF_COMPILE_FAIL CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/*.cpp)
if(NOT DOCUCONF_FILE_INPUTS)
  list(APPEND DOCUCONF_COMPILE_FAIL ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail_env_only/add_file.cpp)
endif()
foreach(src IN LISTS DOCUCONF_COMPILE_FAIL)
  get_filename_component(name ${src} NAME_WE)
  file(STRINGS ${src} first LIMIT_COUNT 1)
  string(REGEX REPLACE "^// Must not compile. Expected: " "" expected "${first}")
  string(REPLACE "(" "\\(" expected "${expected}")
  string(REPLACE ")" "\\)" expected "${expected}")
  add_executable(compile_fail_${name} EXCLUDE_FROM_ALL ${src})
  target_link_libraries(compile_fail_${name} PRIVATE docuconf::docuconf)
  add_test(NAME compile_fail.${name}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target compile_fail_${name})
  set_tests_properties(compile_fail.${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${expected}"
    RESOURCE_LOCK docuconf_build_dir)
endforeach()

# Every C++ block in README.md is compiled (and the test block run), so the
# README cannot drift from the API.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(DOCUCONF_README_DIR ${CMAKE_CURRENT_BINARY_DIR}/readme)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/readme_snippets.py
          ${PROJECT_SOURCE_DIR}/README.md ${DOCUCONF_README_DIR}
  RESULT_VARIABLE readme_result)
if(NOT readme_result EQUAL 0)
  message(FATAL_ERROR "docuconf: cannot extract the README snippets")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/README.md ${CMAKE_CURRENT_SOURCE_DIR}/readme_snippets.py)
file(GLOB readme_programs ${DOCUCONF_README_DIR}/program_*.cpp)
foreach(src IN LISTS readme_programs)
  get_filename_component(name ${src} NAME_WE)
  file(STRINGS ${src} needs_files REGEX "^// docuconf-readme: files$")
  if(needs_files AND NOT DOCUCONF_FILE_INPUTS)
    continue()
  endif()
  add_executable(readme_${name} ${src})
  target_link_libraries(readme_${name} PRIVATE docuconf::docuconf)
endforeach()
file(GLOB readme_tests ${DOCUCONF_README_DIR}/test_*.cpp)
foreach(src IN LISTS readme_tests)
  get_filename_component(name ${src} NAME_WE)
  add_executable(readme_${name} ${src})
  target_link_libraries(readme_${name} PRIVATE docuconf::docuconf GTest::gtest_main)
  add_test(NAME readme.${name} COMMAND readme_${name})
endforeach()
