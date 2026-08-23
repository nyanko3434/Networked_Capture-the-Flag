# One test binary, explicit source lists (root CMakeLists policy: no globs).
# Each module adds its test file here deliberately.
set(CTF_TEST_SOURCES
    test_main.cpp
    test_bytebuffer.cpp
    test_map.cpp
    test_movement.cpp
)

add_executable(ctf_tests ${CTF_TEST_SOURCES})
target_include_directories(ctf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ctf_tests PRIVATE shared Threads::Threads)

add_test(NAME ctf_tests COMMAND ctf_tests)
