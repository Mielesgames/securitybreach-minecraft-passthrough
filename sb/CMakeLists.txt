set(TARGET SecurityBreachMinecraftPassthrough)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/Link.cpp
)

target_include_directories(${TARGET} PRIVATE
    src
    UE4SS_SDK/src
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(${TARGET} PUBLIC UE4SS)