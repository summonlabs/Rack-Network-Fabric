# Test support library: the harness and the child process controller.
add_library(rnf_test_support STATIC
  support/harness.cpp
  support/process.cpp)
target_link_libraries(rnf_test_support PUBLIC RNF::rnf)
target_include_directories(rnf_test_support PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_definitions(rnf_test_support PUBLIC
  RNF_TEST_TOOL_DIR="${CMAKE_BINARY_DIR}")
rnf_configure_target(rnf_test_support)

# No test carries a TIMEOUT property. A test that hangs is a defect in the
# runtime or in the test, and cutting it short would hide exactly the class of
# bug this suite exists to find.
function(rnf_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE rnf_test_support)
  rnf_configure_target(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

rnf_add_test(rnf_test_core        unit/test_core.cpp)
rnf_add_test(rnf_test_evidence    unit/test_evidence.cpp)
rnf_add_test(rnf_test_compose     unit/test_compose.cpp)
rnf_add_test(rnf_test_authority   unit/test_authority.cpp)
rnf_add_test(rnf_test_persist     unit/test_persist.cpp)
rnf_add_test(rnf_test_property    property/test_properties.cpp)
rnf_add_test(rnf_test_adversarial adversarial/test_adversarial.cpp)
rnf_add_test(rnf_test_differential differential/test_differential.cpp)
rnf_add_test(rnf_test_concurrency concurrency/test_concurrency.cpp)
rnf_add_test(rnf_test_integration integration/test_integration.cpp)
if(RNF_BUILD_APPS)
  rnf_add_test(rnf_test_multiprocess multiprocess/test_multiprocess.cpp)
endif()
