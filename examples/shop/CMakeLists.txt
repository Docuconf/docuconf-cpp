cmake_minimum_required(VERSION 3.16)
project(shop LANGUAGES CXX)

# Built on its own, the example uses the SDK in this repository, not a
# published version.
if(NOT TARGET docuconf::docuconf)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. docuconf-sdk)
endif()

# Contract-first: the app loads contract.json, which is compiled in so the
# binary runs from anywhere. Editing contract.json re-runs this step.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/contract.json SHOP_CONTRACT_JSON)
configure_file(contract.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/shop_contract.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS contract.json)

add_library(shop_config STATIC shop.cpp ${CMAKE_CURRENT_BINARY_DIR}/shop_contract.cpp)
target_include_directories(shop_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shop_config PUBLIC docuconf::docuconf)
target_compile_features(shop_config PUBLIC cxx_std_17)

add_executable(shop main.cpp)
target_link_libraries(shop PRIVATE shop_config)

# The test matrix of the README; CI runs it after the build.
add_executable(shop_test shop_test.cpp)
target_link_libraries(shop_test PRIVATE shop_config)
add_test(NAME shop_test COMMAND shop_test)
