set(CACHESCOPE_TEST_SOURCES
  test_json_config.cpp
  test_replacement.cpp
  test_cache.cpp
  test_write_policy.cpp
  test_prefetcher.cpp
  test_dram.cpp
  test_hierarchy.cpp
  test_predictor.cpp
  test_trace.cpp
  test_engine.cpp
)

add_executable(cachescope_tests ${CACHESCOPE_TEST_SOURCES})
target_link_libraries(cachescope_tests PRIVATE cachescope_core GTest::gtest_main)

if(MSVC)
  target_compile_options(cachescope_tests PRIVATE /W4)
  target_compile_definitions(cachescope_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(cachescope_tests PRIVATE -Wall -Wextra)
endif()

# Tests that read fixture traces need to find them regardless of the working
# directory the test runner was launched from.
target_compile_definitions(cachescope_tests PRIVATE
  CACHESCOPE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

include(GoogleTest)
gtest_discover_tests(cachescope_tests
  PROPERTIES TIMEOUT 120
  DISCOVERY_TIMEOUT 60
)
