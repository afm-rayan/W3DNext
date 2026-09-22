add_library(core_config INTERFACE)

include(${CMAKE_CURRENT_LIST_DIR}/config-build.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/config-debug.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/config-memory.cmake)

# Qt configuration for WorldBuilder
if(RTS_ENABLE_WORLDBUILDER_QT)
    include(${CMAKE_CURRENT_LIST_DIR}/qt.cmake)
endif()
