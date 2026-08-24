add_executable(ctf_tests test_placeholder.cpp)
target_link_libraries(ctf_tests PRIVATE shared)

add_test(NAME ctf_tests COMMAND ctf_tests)

# shared/bytebuffer.cpp (implementation_guide.md §1.3): round-trip,
# truncation, overflow — run under ASan/UBSan to catch any out-of-bounds
# access the overflow/underflow guards should be preventing. Compiles
# bytebuffer.cpp directly (rather than linking the pre-built `shared` lib,
# which isn't instrumented) so ASan can see inside it.
add_executable(test_bytebuffer test_bytebuffer.cpp ${CMAKE_SOURCE_DIR}/shared/bytebuffer.cpp)
target_include_directories(test_bytebuffer PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_compile_options(test_bytebuffer PRIVATE -fsanitize=address,undefined -g)
target_link_options(test_bytebuffer PRIVATE -fsanitize=address,undefined)

add_test(NAME test_bytebuffer COMMAND test_bytebuffer)

# shared/map.cpp (implementation_guide.md §1.4): mirror symmetry, AABB
# collision (baseline + X-only + Y-only overlap).
add_executable(test_map test_map.cpp ${CMAKE_SOURCE_DIR}/shared/map.cpp)
target_include_directories(test_map PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_compile_options(test_map PRIVATE -fsanitize=address,undefined -g)
target_link_options(test_map PRIVATE -fsanitize=address,undefined)

add_test(NAME test_map COMMAND test_map)

# shared/movement.cpp (implementation_guide.md §1.5): determinism (README
# §10 named test) and diagonal-speed normalization.
add_executable(test_movement test_movement.cpp
    ${CMAKE_SOURCE_DIR}/shared/movement.cpp
    ${CMAKE_SOURCE_DIR}/shared/map.cpp)
target_include_directories(test_movement PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_compile_options(test_movement PRIVATE -fsanitize=address,undefined -g)
target_link_options(test_movement PRIVATE -fsanitize=address,undefined)

add_test(NAME test_movement COMMAND test_movement)

# shared/protocol.cpp (implementation_guide.md §1.6): round-trip for every
# message type, malformed-UDP-header fuzzing, snapshot per-recipient patch.
add_executable(test_protocol test_protocol.cpp
    ${CMAKE_SOURCE_DIR}/shared/protocol.cpp
    ${CMAKE_SOURCE_DIR}/shared/bytebuffer.cpp)
target_include_directories(test_protocol PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_compile_options(test_protocol PRIVATE -fsanitize=address,undefined -g)
target_link_options(test_protocol PRIVATE -fsanitize=address,undefined)

add_test(NAME test_protocol COMMAND test_protocol)

# shared/net_util.cpp (implementation_guide.md §1.7): non-blocking recv,
# partial-write send_all, TCP reassembly across arbitrary chunk splits
# (the two tests deferred from §1.6 since recv_framed owns framing in this
# codebase), and the 4KB overflow/disconnect signal.
add_executable(test_net_util test_net_util.cpp
    ${CMAKE_SOURCE_DIR}/shared/net_util.cpp
    ${CMAKE_SOURCE_DIR}/shared/protocol.cpp
    ${CMAKE_SOURCE_DIR}/shared/bytebuffer.cpp)
target_include_directories(test_net_util PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_compile_options(test_net_util PRIVATE -fsanitize=address,undefined -g)
target_link_options(test_net_util PRIVATE -fsanitize=address,undefined)

add_test(NAME test_net_util COMMAND test_net_util)
