#include "AllocationCounter.h"

#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

// Read by the replaced operators only, so unread under ThreadSanitizer.
[[maybe_unused]] thread_local bool counting = false;
thread_local std::size_t allocationCount = 0;
thread_local std::size_t deallocationCount = 0;

// The replaced operators' work, compiled with them (not under ThreadSanitizer, whose runtime's
// operators would clash: AllocationCounter.h).
#ifndef ORIZON_UNDER_THREAD_SANITIZER
void* allocate(std::size_t size)
{
    if (counting) {
        ++allocationCount;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    if (counting) {
        ++allocationCount;
    }
    const auto align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1) / align * align;
    // Windows has no aligned_alloc; its aligned blocks come from, and go back to, _aligned_*.
#ifdef _WIN32
    void* memory = _aligned_malloc(rounded == 0 ? align : rounded, align);
#else
    void* memory = std::aligned_alloc(align, rounded == 0 ? align : rounded);
#endif
    if (memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void deallocate(void* memory) noexcept
{
    if (counting && memory != nullptr) {
        ++deallocationCount;
    }
    std::free(memory);
}

void deallocateAligned(void* memory) noexcept
{
    if (counting && memory != nullptr) {
        ++deallocationCount;
    }
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

#endif

} // namespace

namespace orizon::testing {

AllocationCounter::AllocationCounter()
{
    allocationCount = 0;
    deallocationCount = 0;
    counting = true;
}

AllocationCounter::~AllocationCounter()
{
    counting = false;
}

std::size_t AllocationCounter::allocations() const
{
    return allocationCount;
}

std::size_t AllocationCounter::deallocations() const
{
    return deallocationCount;
}

} // namespace orizon::testing

#ifndef ORIZON_UNDER_THREAD_SANITIZER
void* operator new(std::size_t size)
{
    return allocate(size);
}
void* operator new[](std::size_t size)
{
    return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* memory) noexcept
{
    deallocate(memory);
}
void operator delete[](void* memory) noexcept
{
    deallocate(memory);
}
void operator delete(void* memory, std::size_t /*size*/) noexcept
{
    deallocate(memory);
}
void operator delete[](void* memory, std::size_t /*size*/) noexcept
{
    deallocate(memory);
}
void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned(memory);
}
void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned(memory);
}
void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned(memory);
}
void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned(memory);
}
#endif
