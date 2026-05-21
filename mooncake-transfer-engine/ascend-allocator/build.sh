#!/bin/bash
#
# Build the Ascend 2MB-aligned allocator shared library.
#
# Output: $OUTPUT_DIR/ascend_allocator.so
#
# Mirrors mooncake-transfer-engine/nvlink-allocator/build.sh in shape: a
# tiny C++ source compiled to a single .so that exports two extern "C"
# symbols (mc_ascend_malloc / mc_ascend_free) consumable by torch_npu's
# NPUPluggableAllocator.

set -e

# Get output directory from command line argument, default to current directory
OUTPUT_DIR=${1:-.}

# Get include directories from second argument (if provided)
INCLUDE_LIST=""
if [ $# -ge 2 ]; then
    INCLUDE_LIST=${2}
fi

# Resolve CANN toolkit include path. CANN installs under
# /usr/local/Ascend/ascend-toolkit/latest/<arch>-linux/include/acl/acl.h
ASCEND_TOOLKIT_ROOT=$(ls -d /usr/local/Ascend/ascend-toolkit/latest/*-linux 2>/dev/null | head -n1 || true)
if [ -z "${ASCEND_TOOLKIT_ROOT}" ]; then
    echo "Failed to locate CANN toolkit under /usr/local/Ascend/ascend-toolkit/latest/*-linux"
    exit 1
fi
ASCEND_INCLUDE_DIR="${ASCEND_TOOLKIT_ROOT}/include"
ASCEND_LIB_DIR="${ASCEND_TOOLKIT_ROOT}/lib64"

INCLUDE_LIST="${INCLUDE_LIST:+${INCLUDE_LIST} }${ASCEND_INCLUDE_DIR}"

INCLUDE_FLAGS=""
if [ -n "$INCLUDE_LIST" ]; then
    INCLUDE_FLAGS=$(echo "$INCLUDE_LIST" | tr ' ' '\n' | sed 's/^/-I/' | paste -sd' ' -)
fi

echo "Building ascend allocator to: $OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

CPP_FILE=$(dirname $(readlink -f $0))/ascend_allocator.cpp

g++ "$CPP_FILE" -o "$OUTPUT_DIR/ascend_allocator.so" \
    --shared -fPIC -std=c++17 \
    ${INCLUDE_FLAGS} \
    -L"${ASCEND_LIB_DIR}" -lascendcl

if [ $? -eq 0 ]; then
    echo "Successfully built ascend_allocator.so in $OUTPUT_DIR"
else
    echo "Failed to build ascend_allocator.so"
    exit 1
fi
