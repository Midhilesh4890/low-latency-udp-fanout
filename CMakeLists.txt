cmake_minimum_required(VERSION 3.16)

project(PulseFanout VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "PulseFanout currently requires Linux (POSIX shared memory and sendmmsg/recvmmsg)")
endif()

include(CTest)
find_package(Threads REQUIRED)

option(PULSEFANOUT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(pulsefanout_build_options INTERFACE)
target_compile_features(pulsefanout_build_options INTERFACE cxx_std_17)
target_include_directories(
  pulsefanout_build_options
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/harness/include>"
)
target_compile_options(
  pulsefanout_build_options
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
)
target_link_libraries(pulsefanout_build_options INTERFACE Threads::Threads rt)

if(PULSEFANOUT_ENABLE_SANITIZERS)
  target_compile_options(pulsefanout_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(pulsefanout_build_options INTERFACE -fsanitize=address,undefined)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

foreach(app IN ITEMS producer sender receiver consumer)
  add_executable(${app} "harness/src/${app}.cpp")
  target_link_libraries(${app} PRIVATE pulsefanout_build_options)
endforeach()

add_executable(pulsefanout_tests harness/test/test_harness.cpp)
target_link_libraries(pulsefanout_tests PRIVATE pulsefanout_build_options)
target_compile_options(pulsefanout_tests PRIVATE -UNDEBUG)

if(BUILD_TESTING)
  add_test(NAME pulsefanout_unit COMMAND pulsefanout_tests)
  find_package(Python3 COMPONENTS Interpreter)
  find_program(OPENSSL_EXECUTABLE openssl)
  if(Python3_Interpreter_FOUND AND OPENSSL_EXECUTABLE)
    add_test(NAME pulsefanout_integration
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/harness/test/test_integration.py" -v)
    set_tests_properties(pulsefanout_integration PROPERTIES
      ENVIRONMENT "PULSEFANOUT_BIN=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" TIMEOUT 90)
  endif()
endif()

install(TARGETS producer sender receiver consumer RUNTIME DESTINATION bin)
