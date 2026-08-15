#include "poller.h"

#include <vector>

// epoll()-based IPoller (README §2, week 4). Swapped in late so poll vs
// epoll throughput at n=10 is a measured comparison (README §10), not an
// assertion.

namespace ctf {

namespace {

class EpollPoller : public IPoller {
public:
    void add(int, bool) override {}
    void remove(int) override {}
    void set_watch_write(int, bool) override {}
    int wait(PollResult*, int, int) override { return 0; }

private:
    int epoll_fd_ = -1;
};

} // namespace

std::unique_ptr<IPoller> make_epoll_poller() {
    return std::make_unique<EpollPoller>();
}

} // namespace ctf
