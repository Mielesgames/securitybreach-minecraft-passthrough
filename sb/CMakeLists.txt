set(TARGET SecurityBreachMinecraftPassthrough)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/Actors.cpp
    src/Collision.cpp
    src/Input.cpp
    src/Link.cpp
    src/Overlay.cpp
    src/World.cpp
)

target_include_directories(${TARGET} PRIVATE
    src
    UE4SS_SDK/src
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(${TARGET} PUBLIC UE4SS d3d11 dxgi d3dcompiler)