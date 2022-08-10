#pragma once

#include <stdexcept>

/**
 * Lock-free parallel disjoint set data structure (aka UNION-FIND)
 * with path compression and union by rank
 *
 * Supports concurrent find(), same() and unite() calls as described
 * in the paper
 *
 * "Wait-free Parallel Algorithms for the Union-Find Problem"
 * by Richard J. Anderson and Heather Woll
 *
 * This file is a modified version of dset.h from GitHub repository
 * wjakob/dset by Wenzel Jacob.
 *
 * The original implementation by Wenzel Jakob uses 64-bit
 * atomic primitives, and implements the union-find
 * algorithm for 32-bit item ids, which allows up to
 * 2^32^ items. This modified version
 * uses 128-bit primitives for 64-bit item ids,
 * which brings the maximum number of items to 2^64^.
 *
 * See the LICENSE file for licensing information
 * specific to this file.
 *
 * \author Wenzel Jakob
 *
 *
 * USAGE OF GCC ATOMIC PRIMITIVES
 *
 * The implementation in shasta/src/dset64.hpp uses std::atomic<__uint128_t>
 * for lock-free synchronization.
 * On older GCC versions, std::atomic<__uint128_t> is lock-free
 * if compilation is done with -mcx16, which enables the use of the
 * 16-byte (128 bit) compare-and-swap instruction, CMPXCHG16B.
 *
 * Unfortunately, on newer GCC versions, this is no longer true
 * because of gcc bug 80878:
 * https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878
 *
 * As a result, there was a significant performance loss in
 * versions of Shasta built with gcc 7,
 * which is used by default on Ubuntu 18.04, when using
 * machines with large number of virtual processors.
 *
 * It is unlikely that this gcc bug will ever be fixed,
 * and to avoid this performance loss this implementation
 * uses gcc primitive __sync_bool_compare_and_swap instead
 * for lock-free synchronization. When compilation
 * is done with -mcx16 and optimization turned on,
 * this primitive uses the CMPXCHG16B instruction
 * and results in optimal speed.
 *
 * The CMPXCHG16B instruction is available on most modern 64-bit x86 processors.
 * Some older processors that don't implement this instruction
 * will crash with an "Illegal instruction" error
 * upon attempting to run this code.
 *
 */

namespace dsets {

class DisjointSets {
public:

    // Integer type used for synchronization primitives.
    // This must have 128 bits and its atomic type must be lock-free
    // (this is checked in the constructor).
    // See the Compilation/Portability comment above.
    using Aint = __uint128_t;
    static_assert(sizeof(Aint) == 16, "Unexpected size of DisjointSets::Aint.");

    // We use the 128 bits of Aint to hold the parent in the
    // 64 least significant bits and the rank in the most significant bits.
    // Define two masks for these two sets of bits.
    static const Aint parentMask = Aint(~0ULL);
    static const Aint rankMask = parentMask << 64;

    // For memory allocation flexibility, the memory is allocated
    // and owned by the caller.
    DisjointSets(Aint* mData, uint64_t size) : mData(mData), n(size) {
        for (uint64_t i=0; i<size; ++i)
            mData[i] = Aint(i);
    }

    uint64_t find(uint64_t id) const {
        while (id != parent(id)) {
            Aint value = mData[id];
            uint64_t new_parent = parent((uint64_t) value);
            Aint new_value =
                (value & rankMask) | new_parent;
            /* Try to update parent (may fail, that's ok) */
            if (value != new_value)
                __sync_bool_compare_and_swap(&mData[id], value, new_value);
            id = new_parent;
        }
        return id;
    }

    bool same(uint64_t id1, uint64_t id2) const {
        for (;;) {
            id1 = find(id1);
            id2 = find(id2);
            if (id1 == id2)
                return true;
            if (parent(id1) == id1)
                return false;
        }
    }

    uint64_t unite(uint64_t id1, uint64_t id2) {
        for (;;) {
            id1 = find(id1);
            id2 = find(id2);

            if (id1 == id2)
                return id1;

            uint64_t r1 = rank(id1), r2 = rank(id2);

            if (r1 > r2 || (r1 == r2 && id1 < id2)) {
                std::swap(r1, r2);
                std::swap(id1, id2);
            }

            Aint oldEntry = ((Aint) r1 << 64) | id1;
            Aint newEntry = ((Aint) r1 << 64) | id2;

            if (!__sync_bool_compare_and_swap(&mData[id1], oldEntry, newEntry))
                continue;

            if (r1 == r2) {
                oldEntry = ((Aint) r2 << 64) | id2;
                newEntry = ((Aint) (r2+1) << 64) | id2;
                /* Try to update the rank (may fail, that's ok) */
                __sync_bool_compare_and_swap(&mData[id2], oldEntry, newEntry);
            }

            break;
        }
        return id2;
    }

    uint64_t size() const { return n; }

    uint64_t rank(uint64_t id) const {
        return ((uint64_t) (mData[id] >> 64)) & parentMask;
    }

    uint64_t parent(uint64_t id) const {
        return (uint64_t) mData[id];
    }

    // Use memory supplied by the caller, rather than an owned vector.
    // This provides more flexibility in allocating the memory.
    Aint* mData;
    uint64_t n;
};

}
