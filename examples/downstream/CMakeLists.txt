cmake_minimum_required(VERSION 3.25)

# An independent downstream consumer of the installed Rack Network Fabric
# package. This project is deliberately NOT part of the main build: it exists to
# prove that "cmake --install" produces a prefix that a separate project can
# find, link against and run, using nothing but the installed artifacts.

project(rnf_downstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(rnf 1.0 CONFIG REQUIRED)

add_executable(rnf_downstream main.cpp)
target_link_libraries(rnf_downstream PRIVATE RNF::rnf)

if(MSVC)
  target_compile_options(rnf_downstream PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(rnf_downstream PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "rnf package version: ${rnf_VERSION}")
message(STATUS "rnf include dirs   : ${RNF_INCLUDE_DIRS}")
