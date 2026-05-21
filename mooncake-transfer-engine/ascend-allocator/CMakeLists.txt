# Build ascend allocator and output to build directory.
#
# Mirrors mooncake-transfer-engine/nvlink-allocator/CMakeLists.txt. The
# shared library is produced by an external bash script (build.sh) so the
# CMake side can stay independent of the CANN toolchain version.

add_custom_target(build_ascend_allocator DEPENDS transfer_engine)

get_target_property(INCLUDE_DIRS build_ascend_allocator INCLUDE_DIRECTORIES)
string(REPLACE ";" " " INCLUDE_DIRS_STR "${INCLUDE_DIRS}")

if (USE_ASCEND OR USE_ASCEND_DIRECT)
    add_custom_command(
        TARGET build_ascend_allocator
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND bash build.sh ${CMAKE_CURRENT_BINARY_DIR} "${INCLUDE_DIRS_STR}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building ascend allocator to ${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

set_property(TARGET build_ascend_allocator PROPERTY EXCLUDE_FROM_ALL FALSE)
