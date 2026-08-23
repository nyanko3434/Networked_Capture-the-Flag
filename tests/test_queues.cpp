#include <doctest.h>

#include <pthread.h>
#include <atomic>
#include <thread>
#include <vector>

#include "queues.h"

using ctf::InboundCommand;
using ctf::InboundCommandType;
using ctf::InboundQueue;
using ctf::MutexGuard;
using ctf::OutboundEvent;
using ctf::OutboundQueue;

TEST_CASE("inbound queue preserves FIFO order") {
    InboundQueue q;
    for (uint8_t i = 0; i < 10; ++i) {
        InboundCommand cmd;
        cmd.type = InboundCommandType::PlayerInput;
        cmd.player_id = i;
        cmd.input.aim_angle = static_cast<uint16_t>(i * 1000);
        q.push(cmd);
    }

    for (uint8_t i = 0; i < 10; ++i) {
        InboundCommand out;
        REQUIRE(q.pop(out));
        CHECK(out.player_id == i);
        CHECK(out.input.aim_angle == static_cast<uint16_t>(i * 1000));
    }
}

TEST_CASE("pop on empty queue returns false") {
    InboundQueue q;
    InboundCommand out;
    CHECK_FALSE(q.pop(out));
}

TEST_CASE("outbound queue round-trips payloads") {
    OutboundQueue q;
    OutboundEvent ev;
    ev.type = ctf::OutboundEventType::UdpSnapshot;
    ev.payload = {1, 2, 3, 4, 5};
    q.push(ev);

    // A second event keeps ordering.
    OutboundEvent ev2;
    ev2.type = ctf::OutboundEventType::TcpBroadcast;
    ev2.payload = {9};
    q.push(ev2);

    OutboundEvent out;
    REQUIRE(q.pop(out));
    CHECK(out.type == ctf::OutboundEventType::UdpSnapshot);
    CHECK((out.payload == std::vector<uint8_t>{1, 2, 3, 4, 5}));

    REQUIRE(q.pop(out));
    CHECK(out.type == ctf::OutboundEventType::TcpBroadcast);

    CHECK_FALSE(q.pop(out));
}

namespace {

// Locks `m`, then early-returns through the guard destructor.
void lock_and_early_return(pthread_mutex_t& m) {
    MutexGuard guard(m);
    if (true) {
        return; // guard must still unlock
    }
}

} // namespace

TEST_CASE("RAII guard releases the mutex on an early return") {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

    lock_and_early_return(m);

    // If the guard leaked the lock, this would block forever.
    bool acquired = false;
    std::thread t([&] {
        MutexGuard guard(m);
        acquired = true;
    });
    t.join();
    CHECK(acquired);

    pthread_mutex_destroy(&m);
}

TEST_CASE("stress: N producers x 10k pushes all arrive intact") {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 10000;
    InboundQueue q;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                const uint8_t id = static_cast<uint8_t>(p);
                const uint16_t aim =
                    static_cast<uint16_t>((i * 31) & 0xFFFF); // checksum pair
                InboundCommand cmd;
                cmd.type = InboundCommandType::PlayerInput;
                cmd.player_id = id;
                cmd.input.aim_angle = aim;
                cmd.input.buttons = static_cast<uint8_t>(aim & 0xFF);
                q.push(cmd);
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }

    long long counts[kProducers] = {};
    long long torn = 0;
    InboundCommand out;
    while (q.pop(out)) {
        if (out.player_id < kProducers &&
            out.input.buttons ==
                static_cast<uint8_t>(out.input.aim_angle & 0xFF)) {
            counts[out.player_id]++;
        } else {
            ++torn;
        }
    }

    CHECK(torn == 0);
    for (int p = 0; p < kProducers; ++p) {
        CHECK(counts[p] == kPerProducer);
    }
}
