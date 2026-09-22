# w3d_d3d11_smoke - clear-to-color oracle for the D3D11Backend skeleton
# (RENDERER_PORT.md step 3). Compiles D3D11Backend.cpp directly so the test
# stays independent of the full ww3d2 library (and its D3D8 surface).
#
# Deliberately links NO WWVegas static library: everything the backend needs
# from WWVegas is header-only here (Vector3 and friends are inline), and
# pulling in wwlib.lib would drag its __purecall override + GameEngine debug
# symbols (DebugLog/DebugCrash) into a standalone test executable.

set(WWVEGAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Libraries/Source/WWVegas)

add_executable(w3d_d3d11_smoke
    main.cpp
    smoke_backend_stubs.cpp
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11Backend.cpp
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11Backend.h
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11FVF.cpp
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11FVF.h
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11States.cpp
    ${WWVEGAS_DIR}/WW3D2/Backend/D3D11States.h
)

target_include_directories(w3d_d3d11_smoke PRIVATE
    ${WWVEGAS_DIR}
    ${WWVEGAS_DIR}/WW3D2
    ${WWVEGAS_DIR}/WWDebug
    ${WWVEGAS_DIR}/WWLib
    ${WWVEGAS_DIR}/WWMath
    ${WWVEGAS_DIR}/WWSaveLoad
)

target_link_libraries(w3d_d3d11_smoke PRIVATE
    corei_always   # Utility/, Lib/, rts/ include dirs pulled in by WWLib headers
    d3d8lib        # fetched min-dx8-sdk headers (D3DBLEND_* etc.)
    d3d11
    dxgi
    d3dcompiler    # runtime HLSL compilation of the FF-emulation VS/PS
    user32
)
