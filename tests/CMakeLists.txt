# One test binary, explicit source lists (root CMakeLists policy: no globs).
# Each module adds its test file here deliberately.
set(CTF_TEST_SOURCES
    test_main.cpp
    test_bytebuffer.cpp
    test_map.cpp
    test_movement.cpp
    test_protocol.cpp
    test_net_util.cpp
    test_queues.cpp
    test_client_registry.cpp
    test_poller.cpp
)

# Server modules under test, added explicitly as their tests land.
set(CTF_TEST_SERVER_SOURCES
    ../server/queues.cpp
    ../server/client_registry.cpp
    ../server/poller_poll.cpp
)

add_executable(ctf_tests ${CTF_TEST_SOURCES} ${CTF_TEST_SERVER_SOURCES})
target_include_directories(ctf_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/server
)
target_link_libraries(ctf_tests PRIVATE shared Threads::Threads)

add_test(NAME ctf_tests COMMAND ctf_tests)
