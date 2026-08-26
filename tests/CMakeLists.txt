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
    test_lobby.cpp
    test_sim.cpp
    test_broadcast.cpp
    test_net_server.cpp
    test_sim_loop.cpp
    test_net_client.cpp
    test_prediction.cpp
    test_interpolation.cpp
    test_render_policy.cpp
    test_bot_ai.cpp
)

# Server modules under test, added explicitly as their tests land.
set(CTF_TEST_SERVER_SOURCES
    ../server/queues.cpp
    ../server/client_registry.cpp
    ../server/poller_poll.cpp
    ../server/lobby.cpp
    ../server/sim.cpp
    ../server/broadcast.cpp
    ../server/net_server.cpp
    ../server/poller_epoll.cpp
)

# Client modules under test, added explicitly as their tests land
# (implementation_guide.md §3). raylib-free by construction (net_client.cpp
# never includes render.h), so this stays independent of CTF_BUILD_CLIENT.
set(CTF_TEST_CLIENT_SOURCES
    ../client/net_client.cpp
    ../client/prediction.cpp
    ../client/interpolation.cpp
)

add_executable(ctf_tests ${CTF_TEST_SOURCES} ${CTF_TEST_SERVER_SOURCES}
                        ${CTF_TEST_CLIENT_SOURCES})
target_include_directories(ctf_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/client
    ${CMAKE_SOURCE_DIR}/bot
)
target_link_libraries(ctf_tests PRIVATE shared bot_core Threads::Threads)

add_test(NAME ctf_tests COMMAND ctf_tests)
