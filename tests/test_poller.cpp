#include <doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "net_util.h"
#include "poller.h"

using ctf::IPoller;
using ctf::PollResult;

namespace {

// Fills a pipe/socket until writes return EAGAIN, making it not-writable.
void fill_until_full(int fd) {
    std::vector<char> chunk(4096, 'x');
    while (write(fd, chunk.data(), chunk.size()) > 0) {
    }
}

} // namespace

TEST_CASE("empty poller wait respects the timeout without spinning") {
    auto poller = ctf::make_poll_poller();
    PollResult results[4];

    const auto start = std::chrono::steady_clock::now();
    int n = poller->wait(results, 4, 80);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(n == 0);
    // Bounded-timeout sleep: must actually wait ~timeout, proving the
    // implementation blocks in poll() rather than spinning.
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    CHECK(ms >= 70);
}

TEST_CASE("peer write makes the registered fd read-ready") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto poller = ctf::make_poll_poller();
    poller->add(fds[0], false);

    // No data yet -> empty within a short timeout.
    PollResult results[4];
    CHECK(poller->wait(results, 4, 20) == 0);

    REQUIRE(write(fds[1], "ping", 4) == 4);

    int n = poller->wait(results, 4, 100);
    REQUIRE(n == 1);
    CHECK(results[0].fd == fds[0]);
    CHECK(results[0].readable);
    CHECK_FALSE(results[0].writable);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("POLLOUT interest toggles on and off") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    REQUIRE(ctf::set_nonblocking(fds[0]));

    auto poller = ctf::make_poll_poller();

    // Fill the send side so it is currently NOT writable.
    fill_until_full(fds[0]);
    poller->add(fds[0], false);

    PollResult results[4];
    // Not writable, no interest: quiet.
    CHECK(poller->wait(results, 4, 30) == 0);

    // Turn write interest on: still quiet because the buffer is full.
    poller->set_watch_write(fds[0], true);
    CHECK(poller->wait(results, 4, 30) == 0);

    // Peer drains completely: fd becomes write-ready.
    char drain[65536];
    ssize_t n_recv;
    while ((n_recv = recv(fds[1], drain, sizeof(drain), MSG_DONTWAIT)) > 0) {
    }

    int n = poller->wait(results, 4, 100);
    REQUIRE(n >= 1);
    bool saw_writable = false;
    bool still_writable = false;
    for (int i = 0; i < n; ++i) {
        if (results[i].fd == fds[0] && results[i].writable) {
            saw_writable = true;
        }
    }
    CHECK(saw_writable);

    // Toggle back off: no further write-ready reports.
    poller->set_watch_write(fds[0], false);
    n = poller->wait(results, 4, 30);
    still_writable = false;
    for (int i = 0; i < n; ++i) {
        if (results[i].fd == fds[0] && results[i].writable) {
            still_writable = true;
        }
    }
    CHECK(still_writable == false);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("removed fd no longer reported even with pending data") {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto poller = ctf::make_poll_poller();
    poller->add(fds[0], false);
    poller->remove(fds[0]);

    REQUIRE(write(fds[1], "data", 4) == 4);

    PollResult results[4];
    CHECK(poller->wait(results, 4, 50) == 0);

    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("multiple ready fds are all reported in one pass") {
    int a[2], b[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);

    auto poller = ctf::make_poll_poller();
    poller->add(a[0], false);
    poller->add(b[0], false);

    REQUIRE(write(a[1], "x", 1) == 1);
    REQUIRE(write(b[1], "y", 1) == 1);

    PollResult results[8];
    int n = poller->wait(results, 8, 100);
    CHECK(n == 2);

    close(a[0]); close(a[1]);
    close(b[0]); close(b[1]);
}
