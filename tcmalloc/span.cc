// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/span.h"

#include <stdint.h>

#include <algorithm>

#include "tcmalloc/common.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void Span::Sample(StackTrace* stack) {
  ASSERT(!sampled_ && stack);
  sampled_ = 1;
  sampled_stack_ = stack;
  Static::sampled_objects_.prepend(this);
  // LossyAdd is ok: writes to sampled_objects_size_ guarded by pageheap_lock.
  // The cast to value matches Unsample.
  Static::sampled_objects_size_.LossyAdd(
      static_cast<tcmalloc_internal::StatsCounter::Value>(
          AllocatedBytes(*stack, true)));
}

StackTrace* Span::Unsample() {
  if (!sampled_) {
    return nullptr;
  }
  sampled_ = 0;
  StackTrace* stack = sampled_stack_;
  sampled_stack_ = nullptr;
  RemoveFromList();  // from Static::sampled_objects_
  // LossyAdd is ok: writes to sampled_objects_size_ guarded by pageheap_lock.
  // The cast to Value ensures no funny business happens during the negation if
  // sizeof(size_t) != sizeof(Value).
  Static::sampled_objects_size_.LossyAdd(
      -static_cast<tcmalloc_internal::StatsCounter::Value>(
          AllocatedBytes(*stack, true)));
  return stack;
}

double Span::Fragmentation() const {
  const size_t cl = Static::pagemap().sizeclass(first_page_);
  if (cl == 0) {
    // Avoid crashes in production mode code, but report in tests.
    ASSERT(cl != 0);
    return 0;
  }
  const size_t obj_size = Static::sizemap().class_to_size(cl);
  const size_t span_objects = bytes_in_span() / obj_size;
  const size_t live = allocated_;
  if (live == 0) {
    // Avoid crashes in production mode code, but report in tests.
    ASSERT(live != 0);
    return 0;
  }
  // Assume that all in-use objects in this span are spread evenly
  // through this span.  So charge the free space in span evenly
  // to each of the live objects.
  // A note on units here: StackTraceTable::AddTrace(1, *t)
  // represents usage (of whatever kind: heap space, allocation,
  // fragmentation) of 1 object of size t->allocated_size.
  // So we want to report here the number of objects we are "responsible"
  // for pinning - NOT bytes.
  return static_cast<double>(span_objects - live) / live;
}

void Span::AverageFreelistAddedTime(const Span* other) {
  // Do this computation as floating-point to avoid overflowing our uint64_t.
  freelist_added_time_ = static_cast<uint64_t>(
      (static_cast<double>(freelist_added_time_) * num_pages_ +
       static_cast<double>(other->freelist_added_time_) * other->num_pages_) /
      (num_pages_ + other->num_pages_));
}

// Freelist organization.
//
// Partially full spans in CentralFreeList contain a list of free objects
// (freelist). We could use the free objects as linked list nodes and form
// a stack, but since the free objects are not likely to be cache-hot the
// chain of dependent misses is very cache-unfriendly. The current
// organization reduces number of cache misses during push/pop.
//
// Objects in the freelist are represented by 2-byte indices. The index is
// object offset from the span start divided by a constant. For small objects
// (<512) divider is 8, for larger -- 64. This allows to fit all indices into
// 2 bytes.
//
// The freelist has two components. First, we have a small array-based cache
// (4 objects) embedded directly into the Span (cache_ and cache_size_). We can
// access this without touching any objects themselves.
//
// The rest of the freelist is stored as arrays inside free objects themselves.
// We can store object_size / 2 indexes in any object, but this is not always
// sufficient to store the entire contents of a Span in a single object. So we
// reserve the first index slot in an object to form a linked list. We use the
// first object in that list (freelist_) as an array to push/pop from; any
// subsequent objects in the list's arrays are guaranteed to be full.
//
// Graphically this can be depicted as follows:
//
//         freelist_  embed_count_         cache_        cache_size_
// Span: [  |idx|         4          |idx|idx|---|---|        2      ]
//            |
//            \/
//            [idx|idx|idx|idx|idx|---|---|---]  16-byte object
//              |
//              \/
//              [---|idx|idx|idx|idx|idx|idx|idx]  16-byte object
//

Span::ObjIdx Span::PtrToIdx(void* ptr, size_t size) const {
  // Object index is an offset from span start divided by a power-of-two.
  // The divisors are choosen so that
  // (1) objects are aligned on the divisor,
  // (2) index fits into 16 bits and
  // (3) the index of the beginning of all objects is strictly less than
  //     kListEnd (note that we have 256K pages and multi-page spans).
  // For example with 1M spans we need kMultiPageAlignment >= 16.
  // An ASSERT in BuildFreelist() verifies a condition which implies (3).
  uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t off;
  if (size <= SizeMap::kMultiPageSize) {
    // Generally we need to load first_page_ to compute the offset.
    // But first_page_ can be in a different cache line then the fields that
    // we use in FreelistPush otherwise (cache_, cache_size_, freelist_).
    // So we avoid loading first_page_ for smaller sizes that have one page per
    // span, instead we compute the offset by taking low kPageShift bits of the
    // pointer.
    ASSERT(PageIdContaining(ptr) == first_page_);
    off = (p & (kPageSize - 1)) / kAlignment;
  } else {
    off = (p - first_page_.start_uintptr()) / SizeMap::kMultiPageAlignment;
  }
  ObjIdx idx = static_cast<ObjIdx>(off);
  ASSERT(idx != kListEnd);
  ASSERT(idx == off);
  return idx;
}

Span::ObjIdx* Span::IdxToPtr(ObjIdx idx, size_t size) const {
  ASSERT(idx != kListEnd);
  uintptr_t off = first_page_.start_uintptr() +
                  (static_cast<uintptr_t>(idx)
                   << (size <= SizeMap::kMultiPageSize
                           ? kAlignmentShift
                           : SizeMap::kMultiPageAlignmentShift));
  ObjIdx* ptr = reinterpret_cast<ObjIdx*>(off);
  ASSERT(PtrToIdx(ptr, size) == idx);
  return ptr;
}

size_t Span::FreelistPopBatch(void** __restrict batch, size_t N, size_t size) {
  if (ABSL_PREDICT_TRUE(size <= SizeMap::kMultiPageSize)) {
    return FreelistPopBatchSized<Align::SMALL>(batch, N, size);
  } else {
    return FreelistPopBatchSized<Align::LARGE>(batch, N, size);
  }
}

void Span::BuildFreelist(size_t size, size_t count) {
  allocated_ = 0;
  freelist_ = kListEnd;

  ObjIdx idx = 0;
  ObjIdx idxStep = size / kAlignment;
  // Valid objects are {0, idxStep, idxStep * 2, ..., idxStep * (count - 1)}.
  if (size > SizeMap::kMultiPageSize) {
    idxStep = size / SizeMap::kMultiPageAlignment;
  }

  // Verify that the end of the useful portion of the span (and the beginning of
  // the span waste) has an index that doesn't overflow or risk confusion with
  // kListEnd. This is slightly stronger than we actually need (see comment in
  // PtrToIdx for that) but rules out some bugs and weakening it wouldn't
  // actually help. One example of the potential bugs that are ruled out is the
  // possibility of idxEnd (below) overflowing.
  ASSERT(count * idxStep < kListEnd);

  // The index of the end of the useful portion of the span.
  ObjIdx idxEnd = count * idxStep;
  // First, push as much as we can into the cache_.
  int cache_size = 0;
  for (; idx < idxEnd && cache_size < kCacheSize; idx += idxStep) {
    cache_[cache_size] = idx;
    cache_size++;
  }
  cache_size_ = cache_size;

  // Now, build freelist and stack other objects onto freelist objects.
  // Note: we take freelist objects from the beginning and stacked objects
  // from the end. This has a nice property of not paging in whole span at once
  // and not draining whole cache.
  ObjIdx* host = nullptr;  // cached first object on freelist
  const size_t max_embed = size / sizeof(ObjIdx) - 1;
  int embed_count = 0;
  while (idx < idxEnd) {
    // Check the no idx can be confused with kListEnd.
    ASSERT(idx != kListEnd);
    if (host && embed_count != max_embed) {
      // Push onto first object on the freelist.
      embed_count++;
      idxEnd -= idxStep;
      host[embed_count] = idxEnd;
    } else {
      // The first object is full, push new object onto freelist.
      host = IdxToPtr(idx, size);
      host[0] = freelist_;
      freelist_ = idx;
      embed_count = 0;
      idx += idxStep;
    }
  }
  embed_count_ = embed_count;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
