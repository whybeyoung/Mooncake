set(MOONCAKE_CONNECTOR_SOURCES
    kvconnector.cpp
    kvconnector_wrapper.cpp
    thread_pool.cpp)

add_library(mooncake_connector_sdk SHARED ${MOONCAKE_CONNECTOR_SOURCES})
target_include_directories(
  mooncake_connector_sdk
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mooncake-store/include
)
target_link_libraries(
  mooncake_connector_sdk
  PUBLIC
    mooncake_store
    glog::glog
)

set_target_properties(
  mooncake_connector_sdk
  PROPERTIES
    OUTPUT_NAME mooncake_connector_sdk
)

if(BUILD_SHARED_LIBS)
  install(TARGETS mooncake_connector_sdk DESTINATION lib)
endif()
install(
  FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/aiges_type.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kvconnector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kvconnector_wrapper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool.h
  DESTINATION include/mooncake_connector
)
