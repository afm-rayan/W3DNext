# Qt5 configuration for WorldBuilder
# This file is included when RTS_ENABLE_WORLDBUILDER_QT is ON

if(NOT RTS_ENABLE_WORLDBUILDER_QT)
    return()
endif()

# Find Qt5 components
find_package(Qt5 5.15 COMPONENTS Widgets Gui REQUIRED)

# Enable Qt automation
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Create Qt interface library
if(NOT TARGET qt::qt)
    add_library(qt::qt INTERFACE IMPORTED)
    target_link_libraries(qt::qt INTERFACE
        Qt5::Widgets
        Qt5::Gui
    )
    target_include_directories(qt::qt INTERFACE
        ${Qt5Core_INCLUDE_DIRS}
        ${Qt5Widgets_INCLUDE_DIRS}
        ${Qt5Gui_INCLUDE_DIRS}
    )
endif()

# QtDirect3D integration - only build the D11 widget we need
set(BUILD_DIRECT3D11 ON CACHE BOOL "" FORCE)
set(BUILD_DIRECT3D10 OFF CACHE BOOL "" FORCE)
set(BUILD_DIRECT3D12 OFF CACHE BOOL "" FORCE)
set(BUILD_DIRECT3D9 OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_IMGUI OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(USE_CLANG_FORMAT OFF CACHE BOOL "" FORCE)

# Add QtDirect3D source directly since it uses different CMake structure
add_library(QDirect3D11Widget STATIC
    ${CMAKE_SOURCE_DIR}/Dependencies/QtDirect3D/source/QDirect3D11Widget/QDirect3D11Widget.cpp
    ${CMAKE_SOURCE_DIR}/Dependencies/QtDirect3D/source/QDirect3D11Widget/QDirect3D11Widget.h
)

target_include_directories(QDirect3D11Widget PUBLIC
    ${CMAKE_SOURCE_DIR}/Dependencies/QtDirect3D/source/QDirect3D11Widget
)

target_link_libraries(QDirect3D11Widget PUBLIC
    Qt5::Widgets
    Qt5::Gui
    d3d11
    d3dcompiler
    dxgi
)

message(STATUS "Qt5 found: ${Qt5Core_VERSION}")
message(STATUS "QtDirect3D11Widget integrated")
