if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(mooncake_connector_sdk LANGUAGES C CXX)
endif()

set(MOONCAKE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(MOONCAKE_CONNECTOR_SOURCES
    kvconnector.cpp
    kvconnector_wrapper.cpp
    thread_pool.cpp)

set(MOONCAKE_CONNECTOR_PUBLIC_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MOONCAKE_ROOT_DIR}/mooncake-store/include
    ${MOONCAKE_ROOT_DIR}/mooncake-transfer-engine/include
    ${MOONCAKE_ROOT_DIR}/mooncake-common/include
    ${MOONCAKE_ROOT_DIR}/thirdparties/yalantinglibs-0.5.7/include
    ${MOONCAKE_ROOT_DIR}/thirdparties/yalantinglibs-0.5.7/include/ylt/thirdparty)

if(NOT TARGET glog::glog)
  include("${MOONCAKE_ROOT_DIR}/mooncake-common/FindGLOG.cmake")
endif()

if(TARGET mooncake_store_objects)
  set(MOONCAKE_STORE_OBJECT_TARGET mooncake_store_objects)
  set(MOONCAKE_CONNECTOR_EXTRA_LINK_LIBS mooncake_store_objects)
else()
  find_library(
    MOONCAKE_STORE_LIB
    NAMES mooncake_store.a libmooncake_store.a
    PATHS
      ${MOONCAKE_ROOT_DIR}/build/mooncake-store/src
      ${MOONCAKE_ROOT_DIR}/build/lib
      /usr/local/lib
      /usr/lib
    NO_DEFAULT_PATH)

  if(NOT MOONCAKE_STORE_LIB)
    find_library(
      MOONCAKE_STORE_LIB
      NAMES mooncake_store.a libmooncake_store.a)
  endif()

  if(NOT MOONCAKE_STORE_LIB)
    message(FATAL_ERROR
      "Unable to find static mooncake_store. Build from the repository root first or provide an installed archive.")
  endif()

  set(MOONCAKE_CONNECTOR_EXTRA_LINK_LIBS "${MOONCAKE_STORE_LIB}")
endif()

set(MOONCAKE_CONNECTOR_LINK_LIBS ${MOONCAKE_CONNECTOR_EXTRA_LINK_LIBS})
if(TARGET transfer_engine)
  list(APPEND MOONCAKE_CONNECTOR_LINK_LIBS transfer_engine)
endif()
if(TARGET glog::glog)
  list(APPEND MOONCAKE_CONNECTOR_LINK_LIBS glog::glog)
elseif(DEFINED GLOG_TARGET)
  list(APPEND MOONCAKE_CONNECTOR_LINK_LIBS "${GLOG_TARGET}")
endif()

add_library(mooncake_connector_sdk SHARED ${MOONCAKE_CONNECTOR_SOURCES})
if(TARGET mooncake_store_objects)
  target_sources(mooncake_connector_sdk PRIVATE $<TARGET_OBJECTS:mooncake_store_objects>)
endif()
target_include_directories(
  mooncake_connector_sdk
  PUBLIC
    ${MOONCAKE_CONNECTOR_PUBLIC_INCLUDES}
)
target_link_libraries(
  mooncake_connector_sdk
  PUBLIC
    ${MOONCAKE_CONNECTOR_LINK_LIBS}
)
target_compile_features(mooncake_connector_sdk PUBLIC cxx_std_20)

set_target_properties(
  mooncake_connector_sdk
  PROPERTIES
    OUTPUT_NAME mooncake_connector_sdk
)

install(TARGETS mooncake_connector_sdk DESTINATION lib)
install(
  FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/aiges_type.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kvconnector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kvconnector_wrapper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool.h
  DESTINATION include/mooncake_connector
)
