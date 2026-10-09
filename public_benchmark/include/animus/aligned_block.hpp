// SPDX-License-Identifier: Apache-2.0
// aligned_block.hpp -- a 64-byte-aligned, zero-filled heap block for the header-based rings.
//
// SpscQueue, BroadcastRing and the in-process views of the shared-memory rings need caller-supplied memory
// aligned to a cache line. This owns one such block. It allocates (aligned_alloc) in its constructor and frees in
// its destructor, so construct it during setup and destroy it after the measured window, never inside one.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace animus {
namespace bench {

class AlignedBlock {
public:
    explicit AlignedBlock(std::size_t bytes) noexcept
        : size_((bytes + 63u) & ~std::size_t{ 63 }), data_(std::aligned_alloc(64, size_)) {
        if (data_ != nullptr) std::memset(data_, 0, size_);
    }
    ~AlignedBlock() { std::free(data_); }
    AlignedBlock(const AlignedBlock&) = delete;
    AlignedBlock& operator=(const AlignedBlock&) = delete;

    void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    explicit operator bool() const noexcept { return data_ != nullptr; }

private:
    std::size_t size_;
    void* data_;
};

} // namespace bench
} // namespace animus
