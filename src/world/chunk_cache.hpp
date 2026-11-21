#pragma once

// The generated terrain of a column does not depend on who walks into it, but every
// connection used to run the generator and the encoder for the same chunk on its own.
// This holds the last few hundred columns once, so a second viewer costs a copy instead of
// a generation.

#include "world/chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world {

    class ChunkCache {
    public:
        struct Entry {
            Chunk chunk;                       // the pristine generated chunk
            std::vector<std::uint8_t> encoded; // its wire payload, ready to send
        };

        explicit ChunkCache(std::size_t capacity = 96) : capacity_(capacity) {}

        // Most recently used first; a hit also promotes the entry.
        const Entry* find(std::int32_t cx, std::int32_t cz) {
            const auto key = key_of(cx, cz);
            const auto it = index_.find(key);
            if (it == index_.end()) {
                return nullptr;
            }
            order_.splice(order_.begin(), order_, it->second);
            return &it->second->second;
        }

        void put(Chunk chunk, std::vector<std::uint8_t> encoded) {
            const auto key = key_of(chunk.x, chunk.z);
            if (const auto existing = index_.find(key); existing != index_.end()) {
                existing->second->second.chunk = std::move(chunk);
                existing->second->second.encoded = std::move(encoded);
                order_.splice(order_.begin(), order_, existing->second);
                return;
            }

            order_.emplace_front(key, Entry{ std::move(chunk), std::move(encoded) });
            index_[key] = order_.begin();
            while (order_.size() > capacity_) {
                index_.erase(order_.back().first);
                order_.pop_back();
            }
        }

        // A block changed in this column, so the cached copy is stale for its owner.
        void erase(std::int32_t cx, std::int32_t cz) {
            const auto key = key_of(cx, cz);
            const auto it = index_.find(key);
            if (it == index_.end()) {
                return;
            }
            order_.erase(it->second);
            index_.erase(it);
        }

        // The runtime config sizes the cache from the memory budget; shrinking drops the
        // least recently used columns first.
        void set_capacity(std::size_t capacity) {
            capacity_ = capacity == 0 ? 1 : capacity;
            while (order_.size() > capacity_) {
                index_.erase(order_.back().first);
                order_.pop_back();
            }
        }

        std::size_t size() const noexcept { return order_.size(); }
        std::size_t capacity() const noexcept { return capacity_; }

    private:
        static std::uint64_t key_of(std::int32_t cx, std::int32_t cz) noexcept {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                static_cast<std::uint32_t>(cz);
        }

        std::size_t capacity_;
        std::list<std::pair<std::uint64_t, Entry>> order_;
        std::unordered_map<std::uint64_t, decltype(order_)::iterator> index_;
    };

} // namespace terila::world
