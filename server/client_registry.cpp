#include "client_registry.h"

// Owned exclusively by the network thread (README §3.2) — no mutex here by
// design; single-thread access is guaranteed by ownership, and locking
// appears only inside queues.cpp.
//
// Stability invariant: removed entries are tombstoned in place (tcp_fd -1)
// instead of erased, so an entry's address never changes for the registry's
// lifetime — no caller can be left holding a pointer to the wrong player.

#include <vector>

namespace ctf {

namespace {
constexpr int kFreeFd = -1;
} // namespace

// Reserve the hard-capped size up front: storage_ never reallocates, so
// entry addresses are stable for the registry's lifetime.
ClientRegistry::ClientRegistry() {
    storage_.reserve(static_cast<size_t>(config::kMaxPlayers));
}

ClientEntry* ClientRegistry::add(int tcp_fd, const std::string& name) {
    // Reuse the lowest free player_id so ids stay compact after churn.
    for (uint8_t id = 0;; ++id) {
        ClientEntry* existing =
            (id < storage_.size()) ? &storage_[id] : nullptr;
        if (existing != nullptr && existing->tcp_fd != kFreeFd) {
            continue; // id in use
        }

        if (existing == nullptr) {
            if (storage_.size() >= static_cast<size_t>(config::kMaxPlayers)) {
                return nullptr; // full roster: caller sends JOIN_REJECT(full)
            }
            storage_.emplace_back();
            existing = &storage_.back();
        }

        // Live count check across both reused and fresh slots.
        size_t live = 0;
        for (const auto& e : storage_) {
            if (e.tcp_fd != kFreeFd) ++live;
        }
        if (live >= static_cast<size_t>(config::kMaxPlayers)) {
            return nullptr;
        }

        *existing = ClientEntry{};
        existing->player_id = id;
        existing->tcp_fd = tcp_fd;
        existing->name = name;
        return existing;
    }
}

void ClientRegistry::remove(uint8_t player_id) {
    if (player_id < storage_.size() &&
        storage_[player_id].tcp_fd != kFreeFd) {
        storage_[player_id] = ClientEntry{};
        storage_[player_id].tcp_fd = kFreeFd;
    }
}

ClientEntry* ClientRegistry::find_by_id(uint8_t player_id) {
    if (player_id < storage_.size() &&
        storage_[player_id].tcp_fd != kFreeFd) {
        return &storage_[player_id];
    }
    return nullptr;
}

ClientEntry* ClientRegistry::find_by_fd(int tcp_fd) {
    for (auto& e : storage_) {
        if (e.tcp_fd == tcp_fd) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<ClientEntry> ClientRegistry::entries() const {
    std::vector<ClientEntry> live;
    for (const auto& e : storage_) {
        if (e.tcp_fd != kFreeFd) {
            live.push_back(e);
        }
    }
    return live;
}

} // namespace ctf
