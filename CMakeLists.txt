cmake_minimum_required(VERSION 3.25)

project(RackNetworkFabric
  VERSION 1.0.0
  DESCRIPTION "Rack-scoped network state and authority runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Rack-Network-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RNF_BUILD_APPS   "Build the rnfd daemon and rnfctl CLI"       ON)
option(RNF_BUILD_TESTS  "Build the test suite"                       ON)
option(RNF_BUILD_BENCH  "Build the micro benchmark suite"            OFF)
option(RNF_BUILD_EXAMPLES "Build the example programs"               ON)
option(RNF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"    ON)
option(RNF_ENABLE_ASAN  "Build with AddressSanitizer when supported" OFF)
option(RNF_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer when supported" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Warning and sanitizer configuration.
#
# These are applied per target with a function rather than through INTERFACE
# libraries: an INTERFACE target that a static library links PRIVATE would still
# be recorded in the exported target's INTERFACE_LINK_LIBRARIES and would then
# have to be installed, which would leak build-only settings into the package.
# ---------------------------------------------------------------------------
set(RNF_WARNING_FLAGS "")
if(MSVC)
  list(APPEND RNF_WARNING_FLAGS /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
  if(RNF_WARNINGS_AS_ERRORS)
    list(APPEND RNF_WARNING_FLAGS /WX)
  endif()
else()
  list(APPEND RNF_WARNING_FLAGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wformat=2 -Wundef -Wnon-virtual-dtor)
  if(RNF_WARNINGS_AS_ERRORS)
    list(APPEND RNF_WARNING_FLAGS -Werror)
  endif()
endif()

set(RNF_SANITIZER_COMPILE_FLAGS "")
set(RNF_SANITIZER_LINK_FLAGS "")
if(RNF_ENABLE_ASAN OR RNF_ENABLE_UBSAN)
  if(MSVC)
    if(RNF_ENABLE_ASAN)
      list(APPEND RNF_SANITIZER_COMPILE_FLAGS /fsanitize=address /Zi)
      list(APPEND RNF_SANITIZER_LINK_FLAGS /INCREMENTAL:NO)
    endif()
    if(RNF_ENABLE_UBSAN)
      message(FATAL_ERROR
        "MSVC does not provide UndefinedBehaviorSanitizer. Use a Clang or GCC toolchain, "
        "or configure with -DRNF_ENABLE_UBSAN=OFF.")
    endif()
  else()
    if(RNF_ENABLE_ASAN)
      list(APPEND RNF_SANITIZER_COMPILE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
      list(APPEND RNF_SANITIZER_LINK_FLAGS -fsanitize=address)
    endif()
    if(RNF_ENABLE_UBSAN)
      list(APPEND RNF_SANITIZER_COMPILE_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
      list(APPEND RNF_SANITIZER_LINK_FLAGS -fsanitize=undefined)
    endif()
  endif()
endif()

function(rnf_configure_target target)
  target_compile_options(${target} PRIVATE ${RNF_WARNING_FLAGS} ${RNF_SANITIZER_COMPILE_FLAGS})
  if(MSVC)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  if(RNF_SANITIZER_LINK_FLAGS)
    target_link_options(${target} PRIVATE ${RNF_SANITIZER_LINK_FLAGS})
  endif()
endfunction()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RNF_PUBLIC_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/rnf/*.hpp")
file(GLOB_RECURSE RNF_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(rnf STATIC ${RNF_SOURCES})
add_library(RNF::rnf ALIAS rnf)
set_target_properties(rnf PROPERTIES
  OUTPUT_NAME rnf
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(rnf
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(rnf PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(rnf PUBLIC ws2_32)
  target_compile_definitions(rnf PUBLIC
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
  # The C runtime marks fopen/sscanf as deprecated in favour of the _s variants.
  # The runtime uses the standard, portable forms on purpose; the code checks
  # every return value, so the deprecation adds no safety here.
  target_compile_definitions(rnf PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

rnf_configure_target(rnf)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(RNF_BUILD_APPS)
  add_executable(rnfd apps/rnfd/main.cpp)
  target_link_libraries(rnfd PRIVATE RNF::rnf)
  rnf_configure_target(rnfd)

  add_executable(rnfctl apps/rnfctl/main.cpp)
  target_link_libraries(rnfctl PRIVATE RNF::rnf)
  rnf_configure_target(rnfctl)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(RNF_BUILD_EXAMPLES)
  add_executable(rnf_example_embed examples/embed_fabric.cpp)
  target_link_libraries(rnf_example_embed PRIVATE RNF::rnf)
  rnf_configure_target(rnf_example_embed)
endif()

# ---------------------------------------------------------------------------
# Tests and benchmarks
# ---------------------------------------------------------------------------
if(RNF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RNF_BUILD_BENCH)
  add_executable(rnf_bench bench/bench_main.cpp)
  target_link_libraries(rnf_bench PRIVATE RNF::rnf)
  rnf_configure_target(rnf_bench)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS rnf
  EXPORT rnfTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RNF_BUILD_APPS)
  install(TARGETS rnfd rnfctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rnfTargets
  FILE rnfTargets.cmake
  NAMESPACE RNF::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rnf)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rnfConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/rnfConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rnf)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/rnfConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/rnfConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/rnfConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rnf)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/rnf
  OPTIONAL)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Rack Network Fabric ${PROJECT_VERSION}")
message(STATUS "  build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings as errors: ${RNF_WARNINGS_AS_ERRORS}")
message(STATUS "  address sanitizer : ${RNF_ENABLE_ASAN}")
message(STATUS "  apps/tests/bench  : ${RNF_BUILD_APPS}/${RNF_BUILD_TESTS}/${RNF_BUILD_BENCH}")
