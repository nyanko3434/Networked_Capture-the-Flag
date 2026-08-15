#include "poller.h"

#include <vector>

// poll()-based IPoller (README §2: "poll() first, epoll in week 4"). This is
// the default poller for the whole project; the 11 fds involved do not need
// epoll on their own merits — swapping late gives a measurable comparison.

namespace ctf {

namespace {

class PollPoller : public IPoller {
public:
    void add(int, bool) override {}
    void remove(int) override {}
    void set_watch_write(int, bool) override {}
    int wait(PollResult*, int, int) override { return 0; }

private:
    std::vector<int> fds_;
};

} // namespace

std::unique_ptr<IPoller> make_poll_poller() {
    return std::make_unique<PollPoller>();
}

} // namespace ctf
