cmake_minimum_required(VERSION 3.24)
project(SKRClient VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "SKR Client requires Windows")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure Win32 (x86) for the GTA SA module")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- libsodium (pre-built, x86 static) ---
set(SODIUM_ROOT "${CMAKE_SOURCE_DIR}/deps/libsodium")
add_library(sodium STATIC IMPORTED)
set_target_properties(sodium PROPERTIES
  IMPORTED_LOCATION_DEBUG            "${SODIUM_ROOT}/Win32/Debug/v143/static/libsodium.lib"
  IMPORTED_LOCATION_RELEASE          "${SODIUM_ROOT}/Win32/Release/v143/static/libsodium.lib"
  IMPORTED_LOCATION_RELWITHDEBINFO   "${SODIUM_ROOT}/Win32/Release/v143/static/libsodium.lib"
  IMPORTED_LOCATION_MINSIZEREL       "${SODIUM_ROOT}/Win32/Release/v143/static/libsodium.lib"
  INTERFACE_INCLUDE_DIRECTORIES      "${SODIUM_ROOT}/include"
  INTERFACE_COMPILE_DEFINITIONS      "SODIUM_STATIC"
)

# --- Shared library (protocol, crypto, updater) ---
add_library(SKRShared STATIC
  shared/crypto/crypto.cpp
  shared/crypto/ed25519_provider.cpp
  shared/updater/manifest.cpp
  shared/updater/release_gate.cpp
)
target_include_directories(SKRShared PUBLIC ${CMAKE_SOURCE_DIR}/shared)
target_link_libraries(SKRShared PRIVATE bcrypt sodium)

# --- Dear ImGui (for native pause menu) ---
set(SKR_IMGUI_ROOT "${CMAKE_SOURCE_DIR}/deps/imgui")
if(EXISTS "${SKR_IMGUI_ROOT}/imgui.h")
  set(SKR_IMGUI_SOURCES
    "${SKR_IMGUI_ROOT}/imgui.cpp"
    "${SKR_IMGUI_ROOT}/imgui_demo.cpp"
    "${SKR_IMGUI_ROOT}/imgui_draw.cpp"
    "${SKR_IMGUI_ROOT}/imgui_tables.cpp"
    "${SKR_IMGUI_ROOT}/imgui_widgets.cpp"
    "${SKR_IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
    "${SKR_IMGUI_ROOT}/backends/imgui_impl_dx9.cpp"
  )
  set(SKR_IMGUI_AVAILABLE TRUE)
  message(STATUS "Dear ImGui found at ${SKR_IMGUI_ROOT}")
else()
  set(SKR_IMGUI_SOURCES "")
  set(SKR_IMGUI_AVAILABLE FALSE)
  message(STATUS "Dear ImGui not found — native pause menu will not build. Run scripts/setup_imgui.bat")
endif()

option(SKR_EXTERNAL_LOBBY "Experimental WebView2 lobby before GTA startup" OFF)
option(SKR_TEST_CERTIFICATION "TEST_ONLY: Enable experimental Hoodlum certification for controlled runtime testing. NEVER use in production builds." OFF)
option(SKR_OP215 "OP215: Custom vehicle loading (experimental)" OFF)
if(SKR_OP215)
  set(SKR_OP215_SOURCES client/vehicle/custom_vehicle.cpp)
  message(STATUS "OP215 custom vehicle system enabled")
else()
  set(SKR_OP215_SOURCES "")
endif()
set(SKR_LOBBY_MANAGER client/lobby/lobby_manager.cpp)
if(SKR_EXTERNAL_LOBBY)
  set(SKR_LOBBY_MANAGER client/lobby/external_lobby_manager.cpp)
  set(SKR_WEBVIEW2_ROOT "${CMAKE_SOURCE_DIR}/deps/webview2/1.0.2903.40" CACHE PATH "Pinned WebView2 SDK")
  if(NOT EXISTS "${SKR_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
    message(FATAL_ERROR "Install Microsoft.Web.WebView2 1.0.2903.40; see docs/external-launcher.md")
  endif()
endif()
# --- Launcher (with updater + inventory) ---
add_executable(SKRLauncher WIN32
  apps/launcher/main.cpp
  apps/launcher/updater.cpp
  apps/launcher/inventory.cpp
  apps/launcher/game_launcher.cpp
)
# Windows application icon (resource-only; no behavioural change).
if(WIN32)
  target_sources(SKRLauncher PRIVATE apps/launcher/SKRLauncher.rc)
endif()
target_include_directories(SKRLauncher PRIVATE
  ${CMAKE_SOURCE_DIR}/apps/launcher
  ${CMAKE_SOURCE_DIR}/client
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRLauncher PRIVATE user32 bcrypt SKRShared)
if(SKR_EXTERNAL_LOBBY)
  target_sources(SKRLauncher PRIVATE apps/launcher/lobby_window.cpp
    apps/launcher/preflight_window.h
    apps/launcher/preflight_window.cpp
    client/lobby/settings_io.cpp
    client/lobby/server_query.cpp client/lobby/public_directory.cpp shared/performance/fps_settings.cpp
    apps/launcher/preflight/preflight_types.h
    apps/launcher/preflight/game_installation_detector.h
    apps/launcher/preflight/game_installation_detector.cpp
    apps/launcher/preflight/exe_fingerprint.h
    apps/launcher/preflight/exe_fingerprint.cpp
    apps/launcher/preflight/samp_inspector.h
    apps/launcher/preflight/samp_inspector.cpp
    apps/launcher/preflight/dependency_inspector.h
    apps/launcher/preflight/dependency_inspector.cpp
    apps/launcher/preflight/verification_request_guard.h
    apps/launcher/preflight/preflight_controller.h
    apps/launcher/preflight/preflight_controller.cpp)
  target_include_directories(SKRLauncher PRIVATE "${SKR_WEBVIEW2_ROOT}/build/native/include" "${CMAKE_SOURCE_DIR}/client")
  target_link_libraries(SKRLauncher PRIVATE "${SKR_WEBVIEW2_ROOT}/build/native/x86/WebView2LoaderStatic.lib"
    ole32 oleaut32 shell32 shlwapi version ws2_32 winhttp gdi32 advapi32 comctl32 gdiplus)
  target_compile_definitions(SKRLauncher PRIVATE SKR_EXTERNAL_LOBBY=1)
endif()
if(SKR_TEST_CERTIFICATION)
  target_compile_definitions(SKRLauncher PRIVATE SKR_TEST_CERTIFICATION=1)
  message(WARNING ">>> TEST_ONLY BUILD: SKR_TEST_CERTIFICATION is enabled. This build is NOT for production. <<<")
endif()

# --- Client DLL (injected by launcher) ---
add_library(SKRClient SHARED
  client/bootstrap/dllmain.cpp
  client/bootstrap/init_state.cpp
  client/skrac/mock_adapter.cpp
  client/skrac/external_adapter.cpp
  client/skrac/ac_policy.cpp
  client/intro/urban_intro.cpp
  client/intro/launch_cover.cpp
  client/intro/intro_audio.cpp
  client/intro/miniaudio_impl.cpp
  client/render-d3d9/d3d9_hook.cpp
  client/render-d3d9/present_chain.cpp
  client/intro/launch_signals.cpp
  client/render-d3d9/fps_engine.cpp
  shared/performance/fps_settings.cpp
  client/render-d3d9/chat_box.cpp
  client/cef-bridge/cef_bridge.cpp
  client/cef-bridge/cef_plugin_exports.cpp
  ${SKR_LOBBY_MANAGER}
  client/lobby/menu_patch.cpp
  client/lobby/settings_io.cpp
  client/lobby/server_query.cpp
  client/lobby/public_directory.cpp
  client/cef-local/cef_local_loader.cpp
  client/samp-loader/samp_loader.cpp
  client/compatibility/compat_profile.cpp
  client/compatibility/hook_target_resolver.cpp
  client/transport/auth_client.cpp
  client/samp-hooks/samp_hooks.cpp
  client/game-state/game_state.cpp
  client/game-state/radar_hud.cpp
  client/gps/gps_destination.cpp
  client/gps/gps_route.cpp
  client/gps/gps_engine.cpp
  client/gps/gps_renderer.cpp
  client/gps/gps_draw.cpp
  client/gps/gps_flags.cpp
  client/gps/gps_graph.cpp
  client/gps/gps_feedback.cpp
  client/gps/gps_zoom.cpp
  client/gps/gps_hud.cpp
  client/gps/gps_panel.cpp
  client/gps/gps_notify.cpp
  client/gps/gps_checkpoint.cpp
  client/native-api/native_api.cpp
  client/native-api/native_api_ui.cpp
  client/gta-fixes/gta_fixes.cpp
  client/native-pause/native_pause.cpp
  client/native-pause/pause_input.cpp
  client/native-pause/pause_controls.cpp
  client/native-pause/pause_controls_logic.cpp
  client/native-pause/pause_renderer.cpp
  client/native-pause/pause_actions.cpp
  client/native-pause/pause_settings.cpp
  client/native-pause/pause_map.cpp
  client/native-pause/pause_enhanced.cpp
  client/phone/phone_controller.cpp
  client/phone/phone_renderer.cpp
  client/phone/phone_input.cpp
  client/phone/phone_net.cpp
  client/phone/phone_wallpaper.cpp
  client/phone/phone_body.cpp
  client/phone/phone_body_gta.cpp
  client/skrx/skrx_client.cpp
  client/skrx/skrx_vehicles_client.cpp
  ${SKR_IMGUI_SOURCES}
  ${SKR_OP215_SOURCES}
)
if(SKR_EXTERNAL_LOBBY)
  target_compile_definitions(SKRClient PRIVATE SKR_EXTERNAL_LOBBY=1)
endif()
set_target_properties(SKRClient PROPERTIES
  OUTPUT_NAME "SKR.Client" PREFIX ""
)
# Single source of truth for the version shown in the native pause menu.
target_compile_definitions(SKRClient PRIVATE SKR_CLIENT_VERSION="${PROJECT_VERSION}")
target_include_directories(SKRClient PRIVATE
  ${CMAKE_SOURCE_DIR}/client
  ${CMAKE_SOURCE_DIR}/shared
  ${CMAKE_SOURCE_DIR}/deps/miniaudio-0.11.23
)
if(SKR_IMGUI_AVAILABLE)
  target_include_directories(SKRClient PRIVATE
    "${SKR_IMGUI_ROOT}"
    "${SKR_IMGUI_ROOT}/backends"
  )
  target_compile_definitions(SKRClient PRIVATE SKR_IMGUI_AVAILABLE=1)
  # ImGui generates many W4 warnings; suppress them for its sources.
  foreach(imgui_src ${SKR_IMGUI_SOURCES})
    set_source_files_properties(${imgui_src} PROPERTIES COMPILE_OPTIONS "/W0")
  endforeach()
endif()
# Plugin-SDK has GTA 1.0 US declarations (e.g. CTimer), not a standalone FPS unlocker.
# Optional type metadata only; we intentionally do not patch GTA timing offsets.
set(SKR_PLUGIN_SDK_ROOT "${CMAKE_SOURCE_DIR}/deps/plugin-sdk" CACHE PATH "Optional DK22Pac plugin-sdk checkout")
if(EXISTS "${SKR_PLUGIN_SDK_ROOT}/plugin_sa/game_sa/CTimer.h")
  target_include_directories(SKRClient PRIVATE
    "${SKR_PLUGIN_SDK_ROOT}/plugin_sa/game_sa"
    "${SKR_PLUGIN_SDK_ROOT}/plugin_sa"
    "${SKR_PLUGIN_SDK_ROOT}/shared")
  target_compile_definitions(SKRClient PRIVATE SKR_PLUGIN_SDK_AVAILABLE=1)
endif()

target_link_libraries(SKRClient PRIVATE d3d9 dxguid version bcrypt windowscodecs ole32 shell32 ws2_32 winhttp gdi32 SKRShared)

# GPS notifications -> Codex C3 toast renderer. BLOCKED until codex/c3-native-api-ui
# is merged (needs its native_api_ui.h); keep OFF on this tree. docs/GPS_C3_INTEGRATION.md
option(SKR_GPS_C3_BRIDGE "Connect GPS notifications to the C3 Native API toasts (requires C3 merged)" OFF)
if(SKR_GPS_C3_BRIDGE)
  target_sources(SKRClient PRIVATE client/gps/gps_notify_c3.cpp)
  target_compile_definitions(SKRClient PRIVATE SKR_GPS_C3_BRIDGE=1)
endif()
# Third-party single-header implementation; keep its warnings out of the /W4 build.
set_source_files_properties(client/intro/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")

# --- Stage lobby web assets next to SKR.Client.dll under SKR\lobby\ ---
# samp-cef loads the lobby via file:///<dll_dir>/SKR/lobby/lobby.html, so the
# HTML/CSS/JS/background must sit beside the DLL at run time.
set(SKR_LOBBY_WEB_FILES
  client/lobby/lobby.html
  client/lobby/lobby.css
  client/lobby/lobby.js
)
set(SKR_PREFLIGHT_WEB_FILES
  client/preflight/preflight.html
  client/preflight/preflight.css
  client/preflight/preflight.js
)
set(SKR_PINNED_SAMP_SHA256 "B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2")
set(SKR_RUNTIME_STAGE_COMMANDS)
if(EXISTS "${CMAKE_SOURCE_DIR}/samp/samp.dll")
  file(SHA256 "${CMAKE_SOURCE_DIR}/samp/samp.dll" SKR_LOCAL_SAMP_SHA256)
  string(TOUPPER "${SKR_LOCAL_SAMP_SHA256}" SKR_LOCAL_SAMP_SHA256)
  if(SKR_LOCAL_SAMP_SHA256 STREQUAL SKR_PINNED_SAMP_SHA256)
    list(APPEND SKR_RUNTIME_STAGE_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/samp"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/samp/samp.dll"
              "$<TARGET_FILE_DIR:SKRClient>/samp/samp.dll")
  else()
    message(WARNING "samp/samp.dll is present but does not match the pinned R5 SHA-256; it will not be staged")
  endif()
else()
  message(STATUS "Pinned samp/samp.dll is not available; native preflight will remain blocking")
endif()
# Always stage web assets, including builds where no C++ file changed.
add_custom_target(SKRLobbyAssets
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/assets"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/preflight/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SKR_LOBBY_WEB_FILES}
          "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/client/lobby/assets/background.png"
          "${CMAKE_SOURCE_DIR}/client/lobby/assets/logo.png"
          "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/assets/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SKR_PREFLIGHT_WEB_FILES}
          "$<TARGET_FILE_DIR:SKRClient>/SKR/preflight/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/client/preflight/assets/logo.png"
          "$<TARGET_FILE_DIR:SKRClient>/SKR/preflight/assets/"
  # PHONE-M4F: iFruit wallpaper (loaded by client/phone/phone_wallpaper.cpp).
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/phone"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/assets/bg.bmp"
          "$<TARGET_FILE_DIR:SKRClient>/SKR/phone/wallpaper.bmp"
  ${SKR_RUNTIME_STAGE_COMMANDS}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VERBATIM)
add_dependencies(SKRClient SKRLobbyAssets)
if(SKR_EXTERNAL_LOBBY)
  add_dependencies(SKRLauncher SKRLobbyAssets)
endif()

# --- Intro timeline/transport tests (no GTA, no device) ---
add_executable(SKRLobbyTests tests/unit/test_lobby_settings.cpp client/lobby/settings_io.cpp)
add_executable(SKRFpsD3D9Tests tests/unit/test_fps_d3d9.cpp client/render-d3d9/fps_engine.cpp shared/performance/fps_settings.cpp)
target_include_directories(SKRFpsD3D9Tests PRIVATE ${CMAKE_SOURCE_DIR}/client ${CMAKE_SOURCE_DIR}/shared)
target_link_libraries(SKRFpsD3D9Tests PRIVATE d3d9 shell32 ole32)
target_include_directories(SKRLobbyTests PRIVATE ${CMAKE_SOURCE_DIR}/client)
target_link_libraries(SKRLobbyTests PRIVATE shell32 ole32)

add_executable(SKRPreflightTests
  tests/unit/test_preflight_fingerprint.cpp
  apps/launcher/preflight/exe_fingerprint.cpp
  apps/launcher/preflight/game_installation_detector.cpp
  apps/launcher/preflight/samp_inspector.cpp
  client/lobby/settings_io.cpp
)
target_include_directories(SKRPreflightTests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/client)
target_link_libraries(SKRPreflightTests PRIVATE bcrypt shell32 ole32)

add_executable(SKRIntroTests
  tests/unit/test_intro_timeline.cpp
)
target_include_directories(SKRIntroTests PRIVATE ${CMAKE_SOURCE_DIR}/client)

add_executable(SKRNativeInitTests
  tests/unit/test_native_initialization.cpp
  client/bootstrap/init_state.cpp
  client/compatibility/hook_target_resolver.cpp
)
target_include_directories(SKRNativeInitTests PRIVATE ${CMAKE_SOURCE_DIR}/client)
enable_testing()
add_test(NAME SKRNativeInitTests COMMAND SKRNativeInitTests)

add_executable(SKRC162Harness
  tests/unit/test_c162_runtime_harness.cpp
  client/bootstrap/init_state.cpp
  client/compatibility/hook_target_resolver.cpp
)
target_include_directories(SKRC162Harness PRIVATE ${CMAKE_SOURCE_DIR}/client)
add_test(NAME SKRC162Harness COMMAND SKRC162Harness)

add_executable(SKRPauseControlsTests
  tests/unit/test_pause_controls.cpp
  client/native-pause/pause_controls_logic.cpp
)
target_include_directories(SKRPauseControlsTests PRIVATE ${CMAKE_SOURCE_DIR}/client)
add_test(NAME SKRPauseControlsTests COMMAND SKRPauseControlsTests)

# --- SKR Gate (server component library) ---
add_library(SKRGate STATIC
  server/skr-gate/gate.cpp
)
target_include_directories(SKRGate PUBLIC
  ${CMAKE_SOURCE_DIR}/server/skr-gate
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRGate PRIVATE SKRShared)

# --- Auth Service (development stub) ---
add_library(SKRAuth STATIC
  services/auth/auth_service.cpp
)
target_include_directories(SKRAuth PUBLIC
  ${CMAKE_SOURCE_DIR}/services/auth
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRAuth PRIVATE SKRShared)

# --- Ops Library (health, keys, audit, incident response) ---
add_library(SKROps STATIC
  services/ops/health.cpp
  services/ops/key_manager.cpp
  services/ops/audit_log.cpp
  services/ops/incident_response.cpp
)
target_include_directories(SKROps PUBLIC
  ${CMAKE_SOURCE_DIR}/services
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKROps PRIVATE SKRShared)

# --- Unit Tests ---
add_executable(SKRTests
  tests/unit/test_main.cpp
)
target_include_directories(SKRTests PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_link_libraries(SKRTests PRIVATE SKRShared bcrypt sodium)

# --- Ops Unit Tests ---
add_executable(SKROpsTests
  tests/unit/test_ops.cpp
)
target_include_directories(SKROpsTests PRIVATE
  ${CMAKE_SOURCE_DIR}/services
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKROpsTests PRIVATE SKROps SKRShared)

# --- Localhost Test Server ---
add_executable(SKRTestServer
  tools/test_server/main.cpp
  tools/test_server/test_server.cpp
)
target_include_directories(SKRTestServer PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/test_server
  ${CMAKE_SOURCE_DIR}/shared
  ${CMAKE_SOURCE_DIR}/services/auth
  ${CMAKE_SOURCE_DIR}/server/skr-gate
)
target_link_libraries(SKRTestServer PRIVATE SKRShared SKRGate SKRAuth ws2_32)

# --- Smoke Test Client ---
add_executable(SKRSmokeClient
  tools/smoke_client/smoke_client.cpp
)
target_include_directories(SKRSmokeClient PRIVATE
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRSmokeClient PRIVATE SKRShared ws2_32)

# --- Fake Game (DLL smoke test harness) ---
add_executable(SKRFakeGame
  tools/fake_game/fake_game.cpp
)
target_include_directories(SKRFakeGame PRIVATE
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRFakeGame PRIVATE SKRShared ws2_32)

add_executable(SKRLaunchIpcTests tests/unit/test_launch_ipc.cpp)
target_include_directories(SKRLaunchIpcTests PRIVATE ${CMAKE_SOURCE_DIR}/shared)
add_executable(SKRHandoffTests tests/unit/test_handoff.cpp client/render-d3d9/present_chain.cpp)
target_include_directories(SKRHandoffTests PRIVATE ${CMAKE_SOURCE_DIR}/client ${CMAKE_SOURCE_DIR}/shared ${CMAKE_SOURCE_DIR}/apps/launcher)
target_link_libraries(SKRHandoffTests PRIVATE d3d9)
add_executable(SKRLaunchProbe tools/launch_probe.cpp apps/launcher/game_launcher.cpp)
target_include_directories(SKRLaunchProbe PRIVATE ${CMAKE_SOURCE_DIR}/shared ${CMAKE_SOURCE_DIR}/apps/launcher ${CMAKE_SOURCE_DIR}/client)

add_executable(SKRRuntimeHarness
  tools/runtime_harness.cpp
  apps/launcher/game_launcher.cpp
  apps/launcher/preflight/exe_fingerprint.cpp
)
target_include_directories(SKRRuntimeHarness PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/shared
  ${CMAKE_SOURCE_DIR}/apps/launcher
  ${CMAKE_SOURCE_DIR}/client)
target_link_libraries(SKRRuntimeHarness PRIVATE bcrypt)

if(MSVC)
  foreach(target SKRLauncher SKRClient SKRShared SKRGate SKRAuth SKROps SKRTests SKROpsTests SKRIntroTests SKRLobbyTests SKRPreflightTests SKRNativeInitTests SKRTestServer SKRSmokeClient SKRFakeGame SKRLaunchIpcTests SKRRuntimeHarness SKRC162Harness SKRPauseControlsTests)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT)
  endforeach()
endif()


# SKR SA-MP Virtual Filesystem — experimental only.
if(SKR_TEST_CERTIFICATION)
    target_compile_definitions(SKRClient PRIVATE
        SKR_TEST_CERTIFICATION=1
    )

    target_sources(SKRClient PRIVATE
        client/vfs/samp_vfs.cpp
    )
endif()

# OP215: Custom vehicle system (experimental).
if(SKR_OP215)
    add_executable(OP215D4Harness tools/op215_d4_harness.cpp)
    target_include_directories(OP215D4Harness PRIVATE ${CMAKE_SOURCE_DIR}/shared)
    target_compile_definitions(OP215D4Harness PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(OP215D4Harness PRIVATE /W4 /EHsc)

    target_compile_definitions(SKRClient PRIVATE SKR_OP215=1)
    # Stage the custom vehicle IMG archive next to the DLL.
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets/op215/skr_vehicles.img")
      add_custom_command(TARGET SKRClient POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/op215"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/op215/skr_vehicles.img"
                "$<TARGET_FILE_DIR:SKRClient>/SKR/op215/skr_vehicles.img"
        COMMENT "Staging OP215 custom vehicle IMG"
        VERBATIM)
    else()
      message(WARNING "OP215 enabled but assets/op215/skr_vehicles.img not found. Run tools/build_skr_img.ps1 first.")
    endif()
endif()
