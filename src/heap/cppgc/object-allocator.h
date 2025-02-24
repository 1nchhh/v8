// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_OBJECT_ALLOCATOR_H_
#define V8_HEAP_CPPGC_OBJECT_ALLOCATOR_H_

#include "include/cppgc/allocation.h"
#include "include/cppgc/internal/gc-info.h"
#include "include/cppgc/macros.h"
#include "src/base/logging.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/heap-space.h"
#include "src/heap/cppgc/memory.h"
#include "src/heap/cppgc/object-start-bitmap.h"
#include "src/heap/cppgc/raw-heap.h"

namespace cppgc {

namespace internal {
class ObjectAllocator;
class PreFinalizerHandler;
}  // namespace internal

class V8_EXPORT AllocationHandle {
 private:
  AllocationHandle() = default;
  friend class internal::ObjectAllocator;
};

namespace internal {

class StatsCollector;
class PageBackend;

class V8_EXPORT_PRIVATE ObjectAllocator final : public cppgc::AllocationHandle {
 public:
  static constexpr size_t kSmallestSpaceSize = 32;

  ObjectAllocator(RawHeap& heap, PageBackend& page_backend,
                  StatsCollector& stats_collector,
                  PreFinalizerHandler& prefinalizer_handler);

  inline void* AllocateObject(size_t size, GCInfoIndex gcinfo);
  inline void* AllocateObject(size_t size, AlignVal alignment,
                              GCInfoIndex gcinfo);
  inline void* AllocateObject(size_t size, GCInfoIndex gcinfo,
                              CustomSpaceIndex space_index);
  inline void* AllocateObject(size_t size, AlignVal alignment,
                              GCInfoIndex gcinfo, CustomSpaceIndex space_index);

  void ResetLinearAllocationBuffers();

  // Terminate the allocator. Subsequent allocation calls result in a crash.
  void Terminate();

 private:
  bool in_disallow_gc_scope() const;

  // Returns the initially tried SpaceType to allocate an object of |size| bytes
  // on. Returns the largest regular object size bucket for large objects.
  inline static RawHeap::RegularSpaceType GetInitialSpaceIndexForSize(
      size_t size);

  inline void* AllocateObjectOnSpace(NormalPageSpace& space, size_t size,
                                     GCInfoIndex gcinfo);
  inline void* AllocateObjectOnSpace(NormalPageSpace& space, size_t size,
                                     AlignVal alignment, GCInfoIndex gcinfo);
  void* OutOfLineAllocate(NormalPageSpace&, size_t, AlignVal, GCInfoIndex);
  void* OutOfLineAllocateImpl(NormalPageSpace&, size_t, AlignVal, GCInfoIndex);

  void RefillLinearAllocationBuffer(NormalPageSpace&, size_t);
  bool RefillLinearAllocationBufferFromFreeList(NormalPageSpace&, size_t);

  RawHeap& raw_heap_;
  PageBackend& page_backend_;
  StatsCollector& stats_collector_;
  PreFinalizerHandler& prefinalizer_handler_;
};

void* ObjectAllocator::AllocateObject(size_t size, GCInfoIndex gcinfo) {
  DCHECK(!in_disallow_gc_scope());
  const size_t allocation_size =
      RoundUp<kAllocationGranularity>(size + sizeof(HeapObjectHeader));
  const RawHeap::RegularSpaceType type =
      GetInitialSpaceIndexForSize(allocation_size);
  return AllocateObjectOnSpace(NormalPageSpace::From(*raw_heap_.Space(type)),
                               allocation_size, gcinfo);
}

void* ObjectAllocator::AllocateObject(size_t size, AlignVal alignment,
                                      GCInfoIndex gcinfo) {
  DCHECK(!in_disallow_gc_scope());
  const size_t allocation_size =
      RoundUp<kAllocationGranularity>(size + sizeof(HeapObjectHeader));
  const RawHeap::RegularSpaceType type =
      GetInitialSpaceIndexForSize(allocation_size);
  return AllocateObjectOnSpace(NormalPageSpace::From(*raw_heap_.Space(type)),
                               allocation_size, alignment, gcinfo);
}

void* ObjectAllocator::AllocateObject(size_t size, GCInfoIndex gcinfo,
                                      CustomSpaceIndex space_index) {
  DCHECK(!in_disallow_gc_scope());
  const size_t allocation_size =
      RoundUp<kAllocationGranularity>(size + sizeof(HeapObjectHeader));
  return AllocateObjectOnSpace(
      NormalPageSpace::From(*raw_heap_.CustomSpace(space_index)),
      allocation_size, gcinfo);
}

void* ObjectAllocator::AllocateObject(size_t size, AlignVal alignment,
                                      GCInfoIndex gcinfo,
                                      CustomSpaceIndex space_index) {
  DCHECK(!in_disallow_gc_scope());
  const size_t allocation_size =
      RoundUp<kAllocationGranularity>(size + sizeof(HeapObjectHeader));
  return AllocateObjectOnSpace(
      NormalPageSpace::From(*raw_heap_.CustomSpace(space_index)),
      allocation_size, alignment, gcinfo);
}

// static
RawHeap::RegularSpaceType ObjectAllocator::GetInitialSpaceIndexForSize(
    size_t size) {
  static_assert(kSmallestSpaceSize == 32,
                "should be half the next larger size");
  if (size < 64) {
    if (size < kSmallestSpaceSize) return RawHeap::RegularSpaceType::kNormal1;
    return RawHeap::RegularSpaceType::kNormal2;
  }
  if (size < 128) return RawHeap::RegularSpaceType::kNormal3;
  return RawHeap::RegularSpaceType::kNormal4;
}

void* ObjectAllocator::AllocateObjectOnSpace(NormalPageSpace& space,
                                             size_t size, AlignVal alignment,
                                             GCInfoIndex gcinfo) {
  // The APIs are set up to support general alignment. Since we want to keep
  // track of the actual usage there the alignment support currently only covers
  // double-world alignment (8 bytes on 32bit and 16 bytes on 64bit
  // architectures). This is enforced on the public API via static_asserts
  // against alignof(T).
  STATIC_ASSERT(2 * kAllocationGranularity ==
                api_constants::kMaxSupportedAlignment);
  STATIC_ASSERT(kAllocationGranularity == sizeof(HeapObjectHeader));
  DCHECK_EQ(2 * sizeof(HeapObjectHeader), static_cast<size_t>(alignment));
  constexpr size_t kAlignment = 2 * kAllocationGranularity;
  constexpr size_t kAlignmentMask = kAlignment - 1;
  constexpr size_t kPaddingSize = kAlignment - sizeof(HeapObjectHeader);

  NormalPageSpace::LinearAllocationBuffer& current_lab =
      space.linear_allocation_buffer();
  const size_t current_lab_size = current_lab.size();
  // Case 1: The LAB fits the request and the LAB start is already properly
  // aligned.
  bool lab_allocation_will_succeed =
      current_lab_size >= size &&
      (reinterpret_cast<uintptr_t>(current_lab.start() +
                                   sizeof(HeapObjectHeader)) &
       kAlignmentMask) == 0;
  // Case 2: The LAB fits an extended request to manually align the second
  // allocation.
  if (!lab_allocation_will_succeed &&
      (current_lab_size >= (size + kPaddingSize))) {
    void* filler_memory = current_lab.Allocate(kPaddingSize);
    Filler::CreateAt(filler_memory, kPaddingSize);
    lab_allocation_will_succeed = true;
  }
  if (lab_allocation_will_succeed) {
    void* object = AllocateObjectOnSpace(space, size, gcinfo);
    DCHECK_NOT_NULL(object);
    DCHECK_EQ(0u, reinterpret_cast<uintptr_t>(object) & kAlignmentMask);
    return object;
  }
  return OutOfLineAllocate(space, size, alignment, gcinfo);
}

void* ObjectAllocator::AllocateObjectOnSpace(NormalPageSpace& space,
                                             size_t size, GCInfoIndex gcinfo) {
  DCHECK_LT(0u, gcinfo);

  NormalPageSpace::LinearAllocationBuffer& current_lab =
      space.linear_allocation_buffer();
  if (current_lab.size() < size) {
    return OutOfLineAllocate(
        space, size, static_cast<AlignVal>(kAllocationGranularity), gcinfo);
  }

  void* raw = current_lab.Allocate(size);
#if !defined(V8_USE_MEMORY_SANITIZER) && !defined(V8_USE_ADDRESS_SANITIZER) && \
    DEBUG
  // For debug builds, unzap only the payload.
  SetMemoryAccessible(static_cast<char*>(raw) + sizeof(HeapObjectHeader),
                      size - sizeof(HeapObjectHeader));
#else
  SetMemoryAccessible(raw, size);
#endif
  auto* header = new (raw) HeapObjectHeader(size, gcinfo);

  // The marker needs to find the object start concurrently.
  NormalPage::From(BasePage::FromPayload(header))
      ->object_start_bitmap()
      .SetBit<AccessMode::kAtomic>(reinterpret_cast<ConstAddress>(header));

  return header->ObjectStart();
}

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_OBJECT_ALLOCATOR_H_
