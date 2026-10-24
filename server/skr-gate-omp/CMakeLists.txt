cmake_minimum_required(VERSION 3.19)
project(SKRGateOMP LANGUAGES CXX VERSION 0.1.2)
find_package(SQLite3 3.36.0 EXACT REQUIRED)
find_package(Threads REQUIRED)
option(SKR_PHONE_GATEWAY "Offer PHONE Gateway v1; OFF preserves legacy phone transport" ON)

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
    native_api_server.cpp
    phone_backend.cpp
    phone_store.cpp
    phone_worker.cpp
    client_policy.cpp
    skrx_registry_server.cpp
    skrx_gateway_server.cpp
    skrx_vehicles_manager.cpp
    skrx_vehicles_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/crypto/crypto.cpp
)

# AMX headers for the SDK's Pawn component interface (OnGameModeIntro lookup).
# Same Pawn source tree and cell size as the open.mp server build's Pawn component.
set(OMP_PAWN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../server-build/open.mp/lib/pawn/source")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${OMP_NET_DIR}
    ${OMP_PAWN_DIR}
)
target_compile_definitions(${PROJECT_NAME} PRIVATE PAWN_CELL_SIZE=32 HAVE_STDINT_H SKR_PHONE_GATEWAY_ENABLED=$<BOOL:${SKR_PHONE_GATEWAY}>)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OMP-SDK
    OMP-Network
    SQLite3::SQLite3
    Threads::Threads
)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /EHsc)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "SKRGate"
    PREFIX ""
)

add_executable(SKREnhancedTests ../../tests/unit/test_enhanced.cpp)
target_include_directories(SKREnhancedTests PRIVATE ../../shared ${OMP_NET_DIR})
target_link_libraries(SKREnhancedTests PRIVATE OMP-SDK OMP-Network)

add_executable(SKRClientPolicyTests
    ../../tests/unit/test_client_policy.cpp
    client_policy.cpp
)
target_include_directories(SKRClientPolicyTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRRejectionTests ../../tests/unit/test_rejection.cpp)
target_include_directories(SKRRejectionTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRNativeApiTests ../../tests/unit/test_native_api.cpp)
target_include_directories(SKRNativeApiTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRPhoneProtocolTests ../../tests/unit/test_phone_protocol.cpp)
target_include_directories(SKRPhoneProtocolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRPhoneBackendTests ../../tests/unit/test_phone_backend.cpp)
target_include_directories(SKRPhoneBackendTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRXTests
    ../../tests/unit/test_skrx.cpp
    skrx_registry_server.cpp
)
target_include_directories(SKRXTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRXVehiclesTests
    ../../tests/unit/test_skrx_vehicles.cpp
    skrx_vehicles_manager.cpp
)
target_include_directories(SKRXVehiclesTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(SKRGpsTests ../../tests/unit/test_gps.cpp)
target_include_directories(SKRGpsTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRGpsRenderStateTests
    ../../tests/unit/test_gps_render_state.cpp
    ../../client/gps/gps_draw.cpp
)
target_include_directories(SKRGpsRenderStateTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
)
target_link_libraries(SKRGpsRenderStateTests PRIVATE d3d9)

add_executable(SKRGpsRoutingTests
    ../../tests/unit/test_gps_routing.cpp
    ../../client/gps/gps_graph.cpp
    ../../client/gps/gps_route.cpp
    ../../client/gps/gps_engine.cpp
    ../../client/gps/gps_destination.cpp
    ../../client/gps/gps_flags.cpp
    ../../client/gps/gps_renderer.cpp
    ../../client/gps/gps_draw.cpp
    ../../client/gps/gps_feedback.cpp
    ../../client/gps/gps_zoom.cpp
)
target_include_directories(SKRGpsRoutingTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
)
target_link_libraries(SKRGpsRoutingTests PRIVATE d3d9 shell32 ole32)

add_executable(SKRGpsNavigationTests
    ../../tests/unit/test_gps_navigation.cpp
    ../../client/gps/gps_graph.cpp
    ../../client/gps/gps_route.cpp
    ../../client/gps/gps_engine.cpp
    ../../client/gps/gps_destination.cpp
    ../../client/gps/gps_flags.cpp
    ../../client/gps/gps_renderer.cpp
    ../../client/gps/gps_draw.cpp
    ../../client/gps/gps_feedback.cpp
    ../../client/gps/gps_zoom.cpp
)
target_include_directories(SKRGpsNavigationTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
)
target_link_libraries(SKRGpsNavigationTests PRIVATE d3d9 shell32 ole32)

# API-003.2: navigation panel, strokes, dashes, notifications
set(SKR_GPS_TEST_SOURCES
    ../../client/gps/gps_graph.cpp
    ../../client/gps/gps_route.cpp
    ../../client/gps/gps_engine.cpp
    ../../client/gps/gps_destination.cpp
    ../../client/gps/gps_flags.cpp
    ../../client/gps/gps_renderer.cpp
    ../../client/gps/gps_draw.cpp
    ../../client/gps/gps_feedback.cpp
    ../../client/gps/gps_zoom.cpp
    ../../client/gps/gps_notify.cpp
)
add_executable(SKRGpsPanelTests ../../tests/unit/test_gps_panel.cpp ${SKR_GPS_TEST_SOURCES})
target_include_directories(SKRGpsPanelTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
    ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/imgui
)
target_link_libraries(SKRGpsPanelTests PRIVATE d3d9 shell32 ole32)

# API-003.3: GPS world checkpoint state (pure)
add_executable(SKRGpsCheckpointTests ../../tests/unit/test_gps_checkpoint.cpp)
target_include_directories(SKRGpsCheckpointTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../client)

# Offscreen native preview (production panel + stroke code on a HAL device).
# Optional showcase/certification tool: not a test, not shipped with the client.
set(SKR_PREVIEW_IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/imgui)
if(EXISTS "${SKR_PREVIEW_IMGUI}/imgui.h")
    add_executable(SKRGpsHudPreview
        ../../tests/gps_hud_preview.cpp
        ../../client/gps/gps_panel.cpp
        ${SKR_GPS_TEST_SOURCES}
        ${SKR_PREVIEW_IMGUI}/imgui.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_draw.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_tables.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_widgets.cpp
        ${SKR_PREVIEW_IMGUI}/backends/imgui_impl_dx9.cpp
    )
    target_include_directories(SKRGpsHudPreview PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../client
        ${SKR_PREVIEW_IMGUI}
    )
    target_link_libraries(SKRGpsHudPreview PRIVATE d3d9 shell32 ole32 windowscodecs)
endif()

# PHONE-001 M1: SKR Street Phone layout/state tests + offscreen visual prototype.
# Prototype only: nothing here is linked into SKR.Client.dll yet.
add_executable(SKRPhoneTests ../../tests/unit/test_phone.cpp ../../client/phone/phone_controller.cpp)
target_include_directories(SKRPhoneTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../client)
add_executable(SKRPhoneM2Tests ../../tests/unit/test_phone_m2.cpp ../../client/phone/phone_controller.cpp)
target_include_directories(SKRPhoneM2Tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../client)
add_executable(SKRPhoneM3Tests ../../tests/unit/test_phone_m3.cpp ../../client/phone/phone_controller.cpp
    ../../client/phone/phone_body.cpp)
target_include_directories(SKRPhoneM3Tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../client)
add_executable(SKRPhoneM4FTests ../../tests/unit/test_phone_m4f.cpp ../../client/phone/phone_controller.cpp)
target_include_directories(SKRPhoneM4FTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../client
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)
if(EXISTS "${SKR_PREVIEW_IMGUI}/imgui.h")
    add_executable(SKRPhonePreview
        ../../tests/phone_preview.cpp
        ../../client/phone/phone_controller.cpp
        ../../client/phone/phone_renderer.cpp
        ../../client/phone/phone_wallpaper.cpp
        ${SKR_PREVIEW_IMGUI}/imgui.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_draw.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_tables.cpp
        ${SKR_PREVIEW_IMGUI}/imgui_widgets.cpp
        ${SKR_PREVIEW_IMGUI}/backends/imgui_impl_dx9.cpp
    )
    target_include_directories(SKRPhonePreview PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
        ${CMAKE_CURRENT_SOURCE_DIR}/../../client
        ${SKR_PREVIEW_IMGUI}
    )
    target_link_libraries(SKRPhonePreview PRIVATE d3d9 shell32 ole32 windowscodecs)
endif()

# Compile-only check of the GPS -> C3 bridge against the real C3 headers.
# Configure with -DSKR_C3_TREE=<codex/c3-native-api-ui worktree>; nothing links or ships.
if(SKR_C3_TREE AND EXISTS "${SKR_C3_TREE}/client/native-api/native_api_ui.h")
    add_library(SKRGpsC3BridgeCheck OBJECT ../../client/gps/gps_notify_c3.cpp)
    target_include_directories(SKRGpsC3BridgeCheck PRIVATE
        ${SKR_C3_TREE}/client
        ${SKR_C3_TREE}/shared
        ${CMAKE_CURRENT_SOURCE_DIR}/../../client
    )
    target_compile_definitions(SKRGpsC3BridgeCheck PRIVATE SKR_GPS_C3_BRIDGE=1)
endif()

add_executable(SKRRadarTransitionTests ../../tests/unit/test_radar_transitions.cpp)
target_include_directories(SKRRadarTransitionTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

add_executable(SKRPhoneM5Tests ../../tests/unit/test_phone_m5.cpp ../../client/phone/phone_net.cpp)
target_include_directories(SKRPhoneM5Tests PRIVATE ../../client ../../shared)
target_compile_definitions(SKRPhoneM5Tests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

target_include_directories(SKRPhoneTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

target_include_directories(SKRPhoneM2Tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

target_include_directories(SKRPhoneM3Tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

add_executable(SKRPhoneGatewayTests
    ../../tests/unit/test_phone_gateway.cpp
    ../../client/phone/phone_net.cpp
    phone_store.cpp phone_worker.cpp skrx_registry_server.cpp)
target_include_directories(SKRPhoneGatewayTests PRIVATE ../../client ../../shared ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SKRPhoneGatewayTests PRIVATE SQLite3::SQLite3 Threads::Threads)
target_compile_definitions(SKRPhoneGatewayTests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_executable(SKRPhoneWorkerTests ../../tests/unit/test_phone_worker.cpp phone_store.cpp phone_worker.cpp)
target_include_directories(SKRPhoneWorkerTests PRIVATE ../../shared ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SKRPhoneWorkerTests PRIVATE SQLite3::SQLite3 Threads::Threads)
target_compile_definitions(SKRPhoneWorkerTests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
