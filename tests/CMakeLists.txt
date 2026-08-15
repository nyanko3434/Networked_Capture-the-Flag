add_executable(ctf_tests test_placeholder.cpp)
target_link_libraries(ctf_tests PRIVATE shared)

add_test(NAME ctf_tests COMMAND ctf_tests)
