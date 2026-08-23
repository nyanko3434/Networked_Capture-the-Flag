#include "queues.h"

// The ONLY place mutexes appear (README §3.2 lock discipline): push/pop
// copy data in/out under the queue's own mutex and release before anything
// else happens — no syscall, no second lock, ever held here.

namespace ctf {

MutexGuard::MutexGuard(pthread_mutex_t& mutex) : mutex_(mutex) {
    pthread_mutex_lock(&mutex_);
}

MutexGuard::~MutexGuard() {
    pthread_mutex_unlock(&mutex_);
}

void InboundQueue::push(const InboundCommand& cmd) {
    MutexGuard guard(mutex_);
    items_.push_back(cmd);
}

bool InboundQueue::pop(InboundCommand& out) {
    MutexGuard guard(mutex_);
    if (items_.empty()) {
        return false;
    }
    out = items_.front();
    items_.erase(items_.begin());
    return true;
}

void OutboundQueue::push(const OutboundEvent& event) {
    MutexGuard guard(mutex_);
    items_.push_back(event);
}

bool OutboundQueue::pop(OutboundEvent& out) {
    MutexGuard guard(mutex_);
    if (items_.empty()) {
        return false;
    }
    out = items_.front();
    items_.erase(items_.begin());
    return true;
}

} // namespace ctf
