#include "queues.h"

// Scaffolding only — real queue bodies (push/pop under the mutex, per
// README §3.2 lock discipline) are written together when the sim thread is
// split out in week 2 (README §11).

namespace ctf {

MutexGuard::MutexGuard(pthread_mutex_t& mutex) : mutex_(mutex) {
    pthread_mutex_lock(&mutex_);
}

MutexGuard::~MutexGuard() {
    pthread_mutex_unlock(&mutex_);
}

void InboundQueue::push(const InboundCommand&) {}
bool InboundQueue::pop(InboundCommand&) { return false; }

void OutboundQueue::push(const OutboundEvent&) {}
bool OutboundQueue::pop(OutboundEvent&) { return false; }

} // namespace ctf
