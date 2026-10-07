cmake_minimum_required(VERSION 3.16)
project(orders LANGUAGES CXX)

# Built on its own, the example uses the SDK in this repository, not a
# published version.
if(NOT TARGET docuconf::docuconf)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. docuconf-sdk)
endif()

# cpp-httplib is header-only: fetch the header at a pinned tag.
find_package(httplib 0.15 CONFIG QUIET)
if(NOT TARGET httplib::httplib)
  include(FetchContent)
  FetchContent_Declare(cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 5814e121dfb5049f72a5c3956c3c8961b40da78b # v0.18.7
    SOURCE_SUBDIR header-only-no-cmake)
  FetchContent_MakeAvailable(cpp_httplib)
  find_package(Threads REQUIRED)
  add_library(orders_httplib INTERFACE)
  target_include_directories(orders_httplib INTERFACE ${cpp_httplib_SOURCE_DIR})
  target_link_libraries(orders_httplib INTERFACE Threads::Threads)
  add_library(httplib::httplib ALIAS orders_httplib)
endif()

add_executable(orders main.cpp)
target_link_libraries(orders PRIVATE docuconf::docuconf httplib::httplib)
