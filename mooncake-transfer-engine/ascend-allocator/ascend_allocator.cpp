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

    aclError ret = aclrtSetDevice(device);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr,
                "[mc_ascend_allocator] aclrtSetDevice(%d) failed, ret=%d\n",
                device, ret);
        return nullptr;
    }

    void *ptr = nullptr;
    ret = aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_ONLY);
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
    (void)device;
    (void)stream;
    if (ptr == nullptr) {
        return;
    }
    aclError ret = aclrtFree(ptr);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr,
                "[mc_ascend_allocator] aclrtFree(%p) failed, ret=%d\n", ptr,
                ret);
    }
}

}  // extern "C"
