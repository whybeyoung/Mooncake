cmake_minimum_required(VERSION 3.16)
project(mooncake_dummy_test_service CXX C)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Mooncake repo root")
get_filename_component(REPO_ROOT "${REPO_ROOT}" ABSOLUTE)

foreach(prefix
        /opt/homebrew
        /opt/homebrew/opt/glog
        /opt/homebrew/opt/yaml-cpp
        /usr/local
        /usr/local/opt/glog
        /usr/local/opt/yaml-cpp)
  if (EXISTS "${prefix}")
    list(APPEND CMAKE_PREFIX_PATH "${prefix}")
    list(APPEND CMAKE_INCLUDE_PATH "${prefix}/include")
    list(APPEND CMAKE_LIBRARY_PATH "${prefix}/lib")
  endif()
endforeach()

set(GLOBAL_CONFIG "true")
list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/mooncake-common")

include("${REPO_ROOT}/mooncake-common/FindJsonCpp.cmake")

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fPIC")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()
add_compile_definitions(GLOG_USE_GLOG_EXPORT)
add_compile_definitions(CONFIG_ERDMA)

option(BUILD_EXAMPLES "Build examples" OFF)
option(BUILD_UNIT_TESTS "Build unit tests" OFF)
option(WITH_RUST_EXAMPLE "Build rust examples" OFF)
option(USE_CUDA "Enable CUDA support" OFF)
option(USE_MUSA "Enable MUSA support" OFF)
option(USE_HIP "Enable HIP support" OFF)
option(USE_NVMEOF "Enable NVMe-oF support" OFF)
option(USE_TCP "Enable TCP transport" ON)
option(USE_BAREX "Enable barex transport" OFF)
option(USE_ASCEND "Enable Ascend support" OFF)
option(USE_ASCEND_DIRECT "Enable Ascend direct support" OFF)
option(USE_UBSHMEM "Enable ubshmem support" OFF)
option(USE_ASCEND_HETEROGENEOUS "Enable heterogeneous Ascend support" OFF)
option(USE_MNNVL "Enable MNNVL support" OFF)
option(USE_CXL "Enable CXL support" OFF)
option(USE_EFA "Enable EFA support" OFF)
option(USE_ETCD "Enable etcd in transfer engine" OFF)
option(USE_ETCD_LEGACY "Enable legacy etcd" OFF)
option(USE_REDIS "Enable redis metadata" OFF)
option(USE_HTTP "Enable http metadata" ON)
option(WITH_METRICS "Enable metrics" ON)
option(USE_3FS "Enable 3FS backend" OFF)
option(WITH_NVIDIA_PEERMEM "Enable NVIDIA peermem" ON)
option(USE_EVENT_DRIVEN_COMPLETION "Enable event-driven completion" OFF)
option(USE_TENT "Enable TENT" OFF)
option(USE_LRU_MASTER "Enable LRU master" OFF)
option(USE_INTRA_NVLINK "Enable intra-node NVLink" OFF)
option(STORE_USE_ETCD "Build store runtime with etcd support" OFF)

add_compile_definitions(ASIO_SEPARATE_COMPILATION ASIO_DYN_LINK)

find_package(Threads REQUIRED)
find_package(GLOG REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE "true")
find_package(gflags REQUIRED)
find_package(yaml-cpp REQUIRED)

if (NOT TARGET gflags::gflags)
  if (TARGET gflags)
    add_library(gflags::gflags ALIAS gflags)
  elseif (TARGET gflags_shared)
    add_library(gflags::gflags ALIAS gflags_shared)
  elseif (TARGET gflags_static)
    add_library(gflags::gflags ALIAS gflags_static)
  else()
    find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h)
    find_library(GFLAGS_LIBRARY NAMES gflags libgflags)
    if (NOT GFLAGS_INCLUDE_DIR OR NOT GFLAGS_LIBRARY)
      message(FATAL_ERROR "gflags was found, but no usable gflags::gflags target or library path was exported")
    endif()
    add_library(gflags::gflags INTERFACE IMPORTED)
    set_target_properties(gflags::gflags PROPERTIES
                          INTERFACE_INCLUDE_DIRECTORIES "${GFLAGS_INCLUDE_DIR}"
                          INTERFACE_LINK_LIBRARIES "${GFLAGS_LIBRARY}")
  endif()
endif()

find_library(IBVERBS_LIBRARY ibverbs REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)
find_library(XXHASH_LIBRARY xxhash REQUIRED)
find_library(NUMA_LIBRARY numa REQUIRED)
find_library(DL_LIBRARY dl REQUIRED)

set(YLT_ENABLE_IBV ON CACHE BOOL "Enable yalantinglibs ibverbs support" FORCE)
add_subdirectory("${REPO_ROOT}/thirdparties/yalantinglibs-0.5.7"
                 "${CMAKE_BINARY_DIR}/deps/yalantinglibs")

if (USE_TCP)
  add_compile_definitions(USE_TCP)
endif()
if (USE_HTTP)
  add_compile_definitions(USE_HTTP)
endif()
if (WITH_METRICS)
  add_compile_definitions(WITH_METRICS)
endif()
if (WITH_NVIDIA_PEERMEM)
  add_compile_definitions(WITH_NVIDIA_PEERMEM)
endif()
if (USE_EVENT_DRIVEN_COMPLETION)
  add_compile_definitions(USE_EVENT_DRIVEN_COMPLETION)
endif()
if (STORE_USE_ETCD)
  add_compile_definitions(STORE_USE_ETCD)
endif()

add_subdirectory("${REPO_ROOT}/mooncake-asio"
                 "${CMAKE_BINARY_DIR}/deps/mooncake-asio")
add_subdirectory("${REPO_ROOT}/mooncake-transfer-engine"
                 "${CMAKE_BINARY_DIR}/deps/mooncake-transfer-engine")
add_subdirectory("${REPO_ROOT}/mooncake-store/src/cachelib_memory_allocator"
                 "${CMAKE_BINARY_DIR}/deps/cachelib_memory_allocator")

target_include_directories(
    cachelib_memory_allocator
    PUBLIC
        "${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator"
        "${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator/include"
        "${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator/fake_include"
        "${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator/common"
)

if (STORE_USE_ETCD)
  add_compile_definitions(STORE_USE_ETCD)
  add_subdirectory("${REPO_ROOT}/mooncake-common/etcd"
                   "${CMAKE_BINARY_DIR}/deps/mooncake-etcd")
endif()

set(MOONCAKE_STORE_TEST_RUNTIME_SOURCES
    "${REPO_ROOT}/mooncake-store/src/client_buffer.cpp"
    "${REPO_ROOT}/mooncake-store/src/dummy_client.cpp"
    "${REPO_ROOT}/mooncake-store/src/offset_allocator.cpp"
    "${REPO_ROOT}/mooncake-store/src/shm_helper.cpp"
    "${REPO_ROOT}/mooncake-store/src/types.cpp"
    "${REPO_ROOT}/mooncake-store/src/utils.cpp"
)

add_library(mooncake_store_test_runtime
            ${MOONCAKE_STORE_TEST_RUNTIME_SOURCES})

target_compile_options(
    mooncake_store_test_runtime
    PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
)

target_include_directories(
    mooncake_store_test_runtime
    PUBLIC
        "${REPO_ROOT}/mooncake-common/include"
        "${REPO_ROOT}/mooncake-common/etcd"
        "${REPO_ROOT}/mooncake-store/include"
        "${REPO_ROOT}/mooncake-transfer-engine/include"
        "${REPO_ROOT}/mooncake-store/src/cachelib_memory_allocator/include"
)

target_link_libraries(
    mooncake_store_test_runtime
    PUBLIC
        cachelib_memory_allocator
        transfer_engine
        asio_shared
        glog::glog
        ${JSONCPP_TARGET}
        Threads::Threads
        ${IBVERBS_LIBRARY}
        ${ZSTD_LIBRARY}
        ${XXHASH_LIBRARY}
        ${NUMA_LIBRARY}
        ${DL_LIBRARY}
)

if (STORE_USE_ETCD)
  add_dependencies(mooncake_store_test_runtime build_etcd_wrapper)
  target_link_libraries(mooncake_store_test_runtime PUBLIC ${ETCD_WRAPPER_LIB})
endif()

add_executable(
    mooncake_dummy_test_service
    src/main.cpp
    src/http_test_service.cpp
    src/benchmark_runner.cpp
)

target_compile_options(
    mooncake_dummy_test_service
    PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
)

target_include_directories(
    mooncake_dummy_test_service
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src"
)

target_link_libraries(
    mooncake_dummy_test_service
    PRIVATE
        mooncake_store_test_runtime
        gflags::gflags
        glog::glog
)
