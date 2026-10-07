set(TARGET SecurityBreachMinecraftPassthrough)

add_subdirectory(UE4SS_SDK)

add_library(${TARGET} SHARED
    src/dllmain.cpp
)

target_include_directories(${TARGET} PRIVATE
    src
)

target_link_libraries(${TARGET} PUBLIC
    UE4SS
    UE4SS_SDK
)