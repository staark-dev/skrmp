cmake_minimum_required(VERSION 3.19)
project(SKRGateOMP LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# open.mp SDK (cloned into deps/omp-sdk)
set(OMP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/omp-sdk")
if(NOT EXISTS "${OMP_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "open.mp SDK not found at ${OMP_SDK_DIR}\n"
        "Clone it: git clone --recursive https://github.com/openmultiplayer/open.mp-sdk.git deps/omp-sdk")
endif()
add_subdirectory("${OMP_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/omp-sdk")

# open.mp network (cloned into deps/omp-network)
set(OMP_NET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/omp-network")
if(EXISTS "${OMP_NET_DIR}/CMakeLists.txt")
    add_subdirectory("${OMP_NET_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/omp-network")
endif()

# SKR Gate component — shared library (.dll on Windows, .so on Linux)
add_library(${PROJECT_NAME} SHARED
    skr_gate_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/crypto/crypto.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${OMP_NET_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OMP-SDK
    OMP-Network
)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /EHsc)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "SKRGate"
    PREFIX ""
)
