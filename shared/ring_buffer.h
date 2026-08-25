#pragma once

// Fixed-capacity ring buffer for hot-path collections that grow/shrink
// frequently. O(1) push_back, O(1) pop_front, no heap allocation after
// construction. Used by Prediction (history) and Interpolation (snapshots).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ctf {

template <typename T, size_t Capacity>
class RingBuffer {
public:
    RingBuffer() = default;

    void push_back(const T& item) {
        data_[tail_] = item;
        tail_ = (tail_ + 1) % Capacity;
        if (size_ < Capacity) {
            ++size_;
        } else {
            // Overwriting oldest — advance head.
            head_ = (head_ + 1) % Capacity;
        }
    }

    void pop_front() {
        if (size_ == 0) return;
        head_ = (head_ + 1) % Capacity;
        --size_;
    }

    // Drop all elements where pred returns true.
    template <typename Pred>
    void remove_if(Pred pred) {
        size_t new_size = 0;
        for (size_t i = 0; i < size_; ++i) {
            const size_t idx = (head_ + i) % Capacity;
            if (!pred(data_[idx])) {
                const size_t dst = (head_ + new_size) % Capacity;
                if (dst != idx) data_[dst] = data_[idx];
                ++new_size;
            }
        }
        size_ = new_size;
        tail_ = (head_ + size_) % Capacity;
    }

    const T& front() const { return data_[head_]; }
    T& front() { return data_[head_]; }
    const T& back() const { return data_[(tail_ + Capacity - 1) % Capacity]; }
    T& back() { return data_[(tail_ + Capacity - 1) % Capacity]; }

    const T& operator[](size_t i) const { return data_[(head_ + i) % Capacity]; }
    T& operator[](size_t i) { return data_[(head_ + i) % Capacity]; }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    static constexpr size_t capacity() { return Capacity; }

    // Iterator support for range-based for loops.
    class iterator {
    public:
        iterator(RingBuffer* rb, size_t pos) : rb_(rb), pos_(pos) {}
        T& operator*() const { return (*rb_)[pos_]; }
        iterator& operator++() { ++pos_; return *this; }
        bool operator!=(const iterator& o) const { return pos_ != o.pos_; }
    private:
        RingBuffer* rb_;
        size_t pos_;
    };

    class const_iterator {
    public:
        const_iterator(const RingBuffer* rb, size_t pos) : rb_(rb), pos_(pos) {}
        const T& operator*() const { return (*rb_)[pos_]; }
        const_iterator& operator++() { ++pos_; return *this; }
        bool operator!=(const const_iterator& o) const { return pos_ != o.pos_; }
    private:
        const RingBuffer* rb_;
        size_t pos_;
    };

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size_); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }

private:
    std::array<T, Capacity> data_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
};

} // namespace ctf
