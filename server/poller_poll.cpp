#include "poller.h"

#include <poll.h>

#include <algorithm>
#include <vector>

// poll()-based IPoller (README §2: "poll() first, epoll in week 4"). This is
// the default poller for the whole project; the ~11 fds involved do not need
// epoll on their own merits — swapping late gives a measurable comparison.
//
// wait() blocks inside poll() with the caller's timeout; it never spins.

namespace ctf {

namespace {

class PollPoller : public IPoller {
public:
    void add(int fd, bool watch_write) override {
        remove(fd); // idempotent re-add
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN | (watch_write ? POLLOUT : 0);
        fds_.push_back(p);
    }

    void remove(int fd) override {
        fds_.erase(
            std::remove_if(fds_.begin(), fds_.end(),
                           [fd](const pollfd& p) { return p.fd == fd; }),
            fds_.end());
    }

    void set_watch_write(int fd, bool watch_write) override {
        for (auto& p : fds_) {
            if (p.fd == fd) {
                if (watch_write) {
                    p.events |= POLLOUT;
                } else {
                    p.events &= static_cast<short>(~POLLOUT);
                }
            }
        }
    }

    int wait(PollResult* out_results, int max_results, int timeout_ms) override {
        const int ready = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()),
                                 timeout_ms);
        if (ready <= 0) {
            return 0; // timeout or EINTR-spuriously-empty: bounded either way
        }

        int count = 0;
        for (auto& p : fds_) {
            const short significant =
                p.revents & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
            if (significant == 0) {
                continue;
            }
            PollResult r;
            r.fd = p.fd;
            // Errors/hangups surface as readable so the caller's recv path
            // sees them and runs disconnect handling.
            r.readable = (p.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
            r.writable = (p.revents & POLLOUT) != 0;
            out_results[count++] = r;
            if (count == max_results) {
                break;
            }
        }
        return count;
    }

private:
    std::vector<pollfd> fds_;
};

} // namespace

std::unique_ptr<IPoller> make_poll_poller() {
    return std::make_unique<PollPoller>();
}

} // namespace ctf
