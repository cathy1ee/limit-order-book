#pragma once

#include "lob/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// An open-addressing hash map from OrderId to a non-null pointer, used as the
// order book's id index.
//
// std::unordered_map allocates a node per entry and chases a pointer per
// lookup. This keeps every entry in one flat array instead:
//   - linear probing: a key lives at its home slot or the next free one after
//     it, so a lookup scans a few adjacent slots, usually in one cache line
//   - backward-shift deletion: erasing pulls later entries of the same probe
//     run back into the hole, so there are no tombstones and lookups never
//     slow down as orders come and go
//   - Fibonacci hashing (multiply by 2^64 / golden ratio, keep the top bits)
//     spreads sequential order ids, the common case, evenly
//   - the table doubles once it is half full, keeping probe runs short
// A null value marks an empty slot, so values must never be null.
template <typename T>
class FlatIndex {
public:
    explicit FlatIndex(std::size_t initial_capacity = 1024) { reset(initial_capacity); }

    std::size_t size() const { return size_; }

    // The value for id, or nullptr if absent.
    T* find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == nullptr) return nullptr;
            if (s.key == id) return s.value;
        }
    }

    bool contains(OrderId id) const { return find(id) != nullptr; }

    // Inserts id -> value. id must not already be present; value must not be null.
    void insert(OrderId id, T* value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        place(id, value);
        ++size_;
    }

    // Removes id. Returns false if it was absent.
    bool erase(OrderId id) {
        std::size_t hole = home(id);
        while (true) {
            const Slot& s = slots_[hole];
            if (s.value == nullptr) return false;
            if (s.key == id) break;
            hole = (hole + 1) & mask_;
        }

        // Backward shift: walk the rest of the probe run and move back any
        // entry whose home slot is not between the hole and its own slot
        // (cyclically). Such an entry would become unreachable past the hole.
        for (std::size_t next = (hole + 1) & mask_;; next = (next + 1) & mask_) {
            Slot& s = slots_[next];
            if (s.value == nullptr) break;
            const std::size_t h = home(s.key);
            const bool home_in_gap = hole <= next ? (hole < h && h <= next)
                                                  : (hole < h || h <= next);
            if (!home_in_gap) {
                slots_[hole] = s;
                hole = next;
            }
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId key = 0;
        T*      value = nullptr;  // nullptr: empty slot
    };

    std::size_t home(OrderId id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void place(OrderId id, T* value) {
        std::size_t i = home(id);
        while (slots_[i].value != nullptr) i = (i + 1) & mask_;
        slots_[i] = Slot{id, value};
    }

    void reset(std::size_t capacity) {
        capacity = std::bit_ceil(capacity < 8 ? std::size_t{8} : capacity);
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(capacity));
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        reset(old.size() * 2);
        for (const Slot& s : old) {
            if (s.value != nullptr) place(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t       mask_ = 0;
    unsigned          shift_ = 0;
    std::size_t       size_ = 0;
};

}  // namespace lob
