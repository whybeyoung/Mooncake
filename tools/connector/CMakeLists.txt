cmake_minimum_required(VERSION 3.16)
project(mooncake_connector CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(REPO_ROOT "${REPO_ROOT}" ABSOLUTE)

if (PROJECT_IS_TOP_LEVEL)
  message(FATAL_ERROR
    "Please build mooncake_connector from repository root CMake. "
    "Example: cmake -S . -B build -DWITH_STORE=ON -DWITH_TE=ON -DBUILD_CONNECTOR=ON")
endif()

# Sources
set(MOONCAKE_CONNECTOR_SOURCES
    src/kvconnector.cpp
    src/kvconnector_wrapper.cpp)

# Main library
add_library(kvconnector SHARED ${MOONCAKE_CONNECTOR_SOURCES})

target_include_directories(kvconnector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${REPO_ROOT}/mooncake-store/include
    ${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator/include
    ${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator/fake_include
    ${REPO_ROOT}/mooncake-store/include/cachelib_memory_allocator
    ${REPO_ROOT}/mooncake-transfer-engine/include
    ${REPO_ROOT}/mooncake-common/include
    ${REPO_ROOT}/thirdparties/yalantinglibs-0.5.7/include
    ${REPO_ROOT}/thirdparties/yalantinglibs-0.5.7/include/ylt/thirdparty
)

target_link_libraries(kvconnector PRIVATE
    mooncake_store
    transfer_engine
    glog::glog
)

target_compile_features(kvconnector PUBLIC cxx_std_20)

set_target_properties(kvconnector PROPERTIES OUTPUT_NAME kvconnector)

install(TARGETS kvconnector DESTINATION lib)
install(
  FILES
    include/aiges_type.h
    include/kvconnector.h
    include/kvconnector_wrapper.h
  DESTINATION include/mooncake_connector
)
