cmake_minimum_required(VERSION 3.16)
project(ctf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# --- sanitizer presets (README §10: several acceptance criteria require
# ASan/TSan verification). Usage:
#   cmake -DCTF_SANITIZE_ADDRESS=ON ..   # AddressSanitizer + UBSan
#   cmake -DCTF_SANITIZE_THREAD=ON ..    # ThreadSanitizer (queues stress)
option(CTF_SANITIZE_ADDRESS "Build with ASan + UBSan" OFF)
option(CTF_SANITIZE_THREAD "Build with TSan" OFF)
if(CTF_SANITIZE_ADDRESS AND CTF_SANITIZE_THREAD)
    message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(CTF_SANITIZE_ADDRESS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if(CTF_SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# --- raylib (client only), fetched at v5.0 rather than found on the system.
# Server-only builds can skip it entirely: -DCTF_BUILD_CLIENT=OFF (raylib's
# GLFW backend requires X11/RandR dev headers at configure time). ---
option(CTF_BUILD_CLIENT "Build ctf_client, ctf_bot and fetch raylib" ON)

if(CTF_BUILD_CLIENT)
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.0
    GIT_SHALLOW TRUE
)
# raylib 5.0's own CMakeLists.txt predates CMake's removal of <3.5
# compatibility; this only relaxes the policy floor for that subproject.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(raylib)
unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# --- shared: compiled into server, client, and bot (README §4). ---
add_library(shared STATIC
    shared/bytebuffer.cpp
    shared/map.cpp
    shared/movement.cpp
    shared/net_util.cpp
    shared/protocol.cpp
)
target_include_directories(shared PUBLIC ${CMAKE_SOURCE_DIR}/shared)

# --- ctf_server ---
add_executable(ctf_server
    server/main.cpp
    server/poller_poll.cpp
    server/poller_epoll.cpp
    server/net_server.cpp
    server/client_registry.cpp
    server/lobby.cpp
    server/queues.cpp
    server/sim.cpp
    server/broadcast.cpp
    client/interpolation.h
)
target_include_directories(ctf_server PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ctf_server PRIVATE shared Threads::Threads)

# --- client_core: the code shared between the real client and the headless
# bot (net_client, prediction, interpolation). Must stay raylib-free. ---
if(CTF_BUILD_CLIENT)
add_library(client_core STATIC
    client/net_client.cpp
    client/prediction.cpp
    client/interpolation.cpp
)
target_include_directories(client_core PUBLIC ${CMAKE_SOURCE_DIR}/client)
target_link_libraries(client_core PUBLIC shared)

# --- ctf_client ---
add_executable(ctf_client
    client/main.cpp
    client/render.cpp
)
target_link_libraries(ctf_client PRIVATE client_core shared Threads::Threads raylib)

# --- ctf_bot: headless client, reuses client_core, no raylib. ---
add_executable(ctf_bot
    bot/main.cpp
)
target_link_libraries(ctf_bot PRIVATE client_core shared Threads::Threads)
endif()

enable_testing()
add_subdirectory(tests)
