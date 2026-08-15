#pragma once

// IPoller interface (README §4). Abstracts poll() vs epoll() so the network
// thread's event loop can swap implementations for the week 4 benchmark
// (README §2, §11) without touching net_server.cpp.

#include <memory>

namespace ctf {

struct PollResult {
    int fd = -1;
    bool readable = false;
    bool writable = false;
};

class IPoller {
public:
    virtual ~IPoller() = default;

    virtual void add(int fd, bool watch_write) = 0;
    virtual void remove(int fd) = 0;
    virtual void set_watch_write(int fd, bool watch_write) = 0;

    // Blocks until at least one fd is ready or timeout_ms elapses. Writes up
    // to max_results entries into out_results and returns the count.
    virtual int wait(PollResult* out_results, int max_results, int timeout_ms) = 0;
};

std::unique_ptr<IPoller> make_poll_poller();
std::unique_ptr<IPoller> make_epoll_poller();

} // namespace ctf
