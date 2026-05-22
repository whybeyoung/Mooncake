// Copyright 2025 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// 2MB-aligned NPU allocator for CANN HCCL IPC RMA.
//
// Background
// ----------
// CANN HCCL IPC RMA (used by AscendDirectTransport / ADXL) requires every
// (addr, length) handed to RegisterMem to start at a 2 MB page boundary.
// Otherwise halShmemCreateHandle / rtsIpcMemGetExportKey rejects the
// registration and the failure surfaces opaquely later as ADXL Connect
// status 503900 once the peer dials in.
//
// The PyTorch (torch_npu) caching allocator does NOT guarantee 2 MB
// alignment for arbitrary tensor allocations: it allocates large slabs and
// hands out sub-blocks at much smaller granularity, so individual
// tensor data_ptr() values are essentially never 2 MB aligned.
//
// Strategy (mirrors GPU's mc_nvlink_malloc / mc_nvlink_free)
// ----------------------------------------------------------
// Expose two extern "C" symbols that match torch's pluggable-allocator ABI:
//
//     void *mc_ascend_malloc(size_t size, int device, void *stream);
//     void  mc_ascend_free  (void *ptr, size_t size, int device, void *stream);
//
// Internally these go through `aclrtMalloc(..., ACL_MEM_MALLOC_HUGE_ONLY)`
// which only allocates from the 2 MB huge-page pool and therefore returns a
// 2 MB-aligned address. Any tensor created while torch_npu's
// NPUPluggableAllocator is bound to this .so is automatically registrable
// with ADXL without any caller-side alignment hack.
//
// HUGE_ONLY is the only supported policy: the entire purpose of this
// allocator is to satisfy the 2 MB-alignment contract, so there is no
// useful fallback. If callers do not want 2 MB alignment, they should
// simply not bind this allocator (torch_npu's default allocator continues
// to work as before).

#include <acl/acl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace {

// RAII guard: switch the calling thread's NPU device to `target_device`
// for the duration of an aclrtMalloc / aclrtFree call, then restore the
// previous device on scope exit. Without this guard, mc_ascend_malloc /
// mc_ascend_free would silently mutate the caller's current device, which
// is observable by every subsequent ACL call on this thread.
class DeviceGuard {
   public:
    explicit DeviceGuard(int target_device)
        : target_device_(target_device), ok_(false) {
        int cur = -1;
        aclError ret = aclrtGetDevice(&cur);
        if (ret == ACL_ERROR_NONE) {
            prev_device_ = cur;
            have_prev_ = true;
            if (cur == target_device_) {
                // Already on target — no switch needed, nothing to restore.
                ok_ = true;
                return;
            }
        }
        ret = aclrtSetDevice(target_device_);
        if (ret != ACL_ERROR_NONE) {
            fprintf(stderr,
                    "[mc_ascend_allocator] aclrtSetDevice(%d) failed, ret=%d\n",
                    target_device_, ret);
            return;
        }
        switched_ = true;
        ok_ = true;
    }

    ~DeviceGuard() {
        if (!switched_) {
            return;
        }
        // Only restore if we actually changed and have a meaningful prev.
        if (!have_prev_ || prev_device_ < 0) {
            return;
        }
        aclError ret = aclrtSetDevice(prev_device_);
        if (ret != ACL_ERROR_NONE) {
            fprintf(stderr,
                    "[mc_ascend_allocator] restore aclrtSetDevice(%d)"
                    " failed, ret=%d\n",
                    prev_device_, ret);
        }
    }

    bool ok() const { return ok_; }

    DeviceGuard(const DeviceGuard &) = delete;
    DeviceGuard &operator=(const DeviceGuard &) = delete;

   private:
    int target_device_;
    int prev_device_ = -1;
    bool have_prev_ = false;
    bool switched_ = false;
    bool ok_;
};

}  // namespace

extern "C" {

// Signature matches torch_npu.npu.NPUPluggableAllocator (and torch's
// CUDAPluggableAllocator): (size, device, stream) -> ptr.
//
// The `stream` argument is currently unused — aclrtMalloc itself is
// synchronous w.r.t. the current device context. We keep the parameter so
// the ABI stays compatible with PyTorch's pluggable-allocator contract.
void *mc_ascend_malloc(size_t size, int device, void *stream) {
    (void)stream;

    if (size == 0) {
        return nullptr;
    }

    DeviceGuard guard(device);
    if (!guard.ok()) {
        return nullptr;
    }

    void *ptr = nullptr;
    aclError ret = aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_ONLY);
    if (ret != ACL_ERROR_NONE || ptr == nullptr) {
        fprintf(stderr,
                "[mc_ascend_allocator] aclrtMalloc(size=%zu, device=%d,"
                " HUGE_ONLY) failed, ret=%d\n",
                size, device, ret);
        return nullptr;
    }
    return ptr;
}

void mc_ascend_free(void *ptr, size_t size, int device, void *stream) {
    (void)size;
    (void)stream;
    if (ptr == nullptr) {
        return;
    }

    DeviceGuard guard(device);
    if (!guard.ok()) {
        // Best-effort: still attempt the free on the current device. Worst
        // case the underlying CANN call reports an error which we surface
        // via fprintf below.
    }

    aclError ret = aclrtFree(ptr);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr,
                "[mc_ascend_allocator] aclrtFree(%p, device=%d) failed,"
                " ret=%d\n",
                ptr, device, ret);
    }
}

}  // extern "C"
