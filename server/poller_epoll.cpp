#include "poller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

// epoll()-backed IPoller (README §2, §4.1): same interface, swappable at
// runtime via --poller so the week-4-style comparison runs both from one
// binary. wait() blocks in epoll_wait with the caller's timeout; no spin.

namespace ctf {

namespace {

class EpollPoller : public IPoller {
public:
    ~EpollPoller() override {
        if (epfd_ >= 0) close(epfd_);
    }

    void add(int fd, bool watch_write) override {
        remove(fd); // idempotent re-add
        epoll_event ev{};
        ev.events = EPOLLIN | (watch_write ? EPOLLOUT : 0);
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
            fds_.push_back(fd);
        }
    }

    void remove(int fd) override {
        if (epfd_ >= 0) {
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        fds_.erase(
            std::remove(fds_.begin(), fds_.end(), fd), fds_.end());
    }

    void set_watch_write(int fd, bool watch_write) override {
        epoll_event ev{};
        ev.events = EPOLLIN | (watch_write ? EPOLLOUT : 0);
        ev.data.fd = fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }

    int wait(PollResult* out_results, int max_results,
             int timeout_ms) override {
        if (fds_.empty()) {
            // epoll_wait on an empty set returns immediately; sleep the
            // timeout instead so callers get bounded-timeout semantics.
            usleep(static_cast<useconds_t>(timeout_ms * 1000));
            return 0;
        }
        epoll_event events[32];
        const int n =
            ::epoll_wait(epfd_, events,
                         static_cast<int>(sizeof(events) / sizeof(events[0])) <
                                 max_results
                             ? static_cast<int>(sizeof(events) /
                                                sizeof(events[0]))
                             : max_results,
                         timeout_ms);
        int count = 0;
        for (int i = 0; i < n; ++i) {
            PollResult r;
            r.fd = events[i].data.fd;
            r.readable =
                (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0;
            r.writable = (events[i].events & EPOLLOUT) != 0;
            out_results[count++] = r;
            if (count == max_results) break;
        }
        return count;
    }

private:
    int epfd_ = epoll_create1(0);
    std::vector<int> fds_;
};

} // namespace

std::unique_ptr<IPoller> make_epoll_poller() {
    return std::make_unique<EpollPoller>();
}

} // namespace ctf
