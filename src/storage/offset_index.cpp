#include "offset_index.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace tributary {

OffsetIndex::OffsetIndex(int64_t segment_base_offset, uint32_t interval_bytes)
    : segment_base_offset_(segment_base_offset),
      interval_bytes_(interval_bytes) {}

void OffsetIndex::MaybeAdd(int64_t batch_base_offset, uint64_t position) {
    if (entries_.empty()) {
        entries_.push_back({batch_base_offset, position});
        return;
    }

    const Entry& last = entries_.back();
    assert(batch_base_offset > last.offset && position > last.position);

    if (position - last.position >= interval_bytes_) {
        entries_.push_back({batch_base_offset, position});
    }
}

OffsetIndex::Entry OffsetIndex::Lookup(int64_t offset) const {
    // upper_bound finds the first entry with an offset greater than the target
    const auto it = std::upper_bound(
        entries_.begin(), entries_.end(), offset,
        // comparator takes (target, entry)
        [](int64_t target, const Entry& e) { return target < e.offset; });

    // If first entry in segment, add with position 0
    if (it == entries_.begin()) return Entry{segment_base_offset_, 0};
    return *std::prev(it);
}

void OffsetIndex::TruncateTo(uint64_t position) {
    // lower_bound finds the first entry at or after position. Everything beyond
    // that points to bytes that do not exist.
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), position,
        // comparator takes (entry, target)
        [](const Entry& e, uint64_t target) { return e.position < target; });

    // erase all entries past that point
    entries_.erase(it, entries_.end());
}

}  // namespace tributary
