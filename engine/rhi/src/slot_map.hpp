#pragma once

// Generational slot table behind RHI handles. Stale handles (destroyed, or
// from a reused slot) resolve to nullptr instead of the wrong object.

#include <veach/rhi/types.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace veach::rhi {

template <class T, class H>
class SlotMap {
public:
    H insert(T value) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& slot = slots_[index];
        slot.value.emplace(std::move(value));
        ++slot.generation;
        return H{index, slot.generation};
    }

    [[nodiscard]] T* get(H handle) {
        if (handle.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        return slot.generation == handle.generation && slot.value ? &*slot.value : nullptr;
    }

    [[nodiscard]] const T* get(H handle) const { return const_cast<SlotMap*>(this)->get(handle); }

    bool erase(H handle) {
        if (!get(handle)) {
            return false;
        }
        slots_[handle.index].value.reset();
        free_.push_back(handle.index);
        return true;
    }

    template <class F>
    void for_each(F&& fn) {
        for (Slot& slot : slots_) {
            if (slot.value) {
                fn(*slot.value);
            }
        }
    }

    void clear() {
        slots_.clear();
        free_.clear();
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 0;
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
};

}  // namespace veach::rhi
