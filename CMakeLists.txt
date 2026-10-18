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

# --- Launcher (with updater + inventory) ---
add_executable(SKRLauncher WIN32
  apps/launcher/main.cpp
  apps/launcher/updater.cpp
  apps/launcher/inventory.cpp
  apps/launcher/game_launcher.cpp
)
target_include_directories(SKRLauncher PRIVATE
  ${CMAKE_SOURCE_DIR}/apps/launcher
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRLauncher PRIVATE user32 bcrypt SKRShared)

# --- Client DLL (injected by launcher) ---
add_library(SKRClient SHARED
  client/bootstrap/dllmain.cpp
  client/skrac/mock_adapter.cpp
  client/skrac/external_adapter.cpp
  client/skrac/ac_policy.cpp
  client/intro/urban_intro.cpp
  client/intro/intro_audio.cpp
  client/intro/miniaudio_impl.cpp
  client/render-d3d9/d3d9_hook.cpp
  client/render-d3d9/chat_box.cpp
  client/cef-bridge/cef_bridge.cpp
  client/cef-bridge/cef_plugin_exports.cpp
  client/lobby/lobby_manager.cpp
  client/lobby/menu_patch.cpp
  client/lobby/settings_io.cpp
  client/lobby/server_query.cpp
  client/lobby/public_directory.cpp
  client/cef-local/cef_local_loader.cpp
  client/samp-loader/samp_loader.cpp
  client/compatibility/compat_profile.cpp
  client/transport/auth_client.cpp
  client/samp-hooks/samp_hooks.cpp
  client/game-state/game_state.cpp
  client/gta-fixes/gta_fixes.cpp
  client/escape-menu/escape_menu.cpp
)
set_target_properties(SKRClient PROPERTIES
  OUTPUT_NAME "SKR.Client" PREFIX ""
)
target_include_directories(SKRClient PRIVATE
  ${CMAKE_SOURCE_DIR}/client
  ${CMAKE_SOURCE_DIR}/shared
  ${CMAKE_SOURCE_DIR}/deps/miniaudio-0.11.23
)
target_link_libraries(SKRClient PRIVATE d3d9 dxguid version bcrypt windowscodecs ole32 shell32 ws2_32 winhttp SKRShared)
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
# Always stage web assets, including builds where no C++ file changed.
add_custom_target(SKRLobbyAssets
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/assets"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SKRClient>/SKR/escape_menu"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SKR_LOBBY_WEB_FILES}
          "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/client/lobby/assets/background.png"
          "${CMAKE_SOURCE_DIR}/client/lobby/assets/logo.png"
          "$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/assets/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/client/hud/escape_menu/escape_menu.html"
          "$<TARGET_FILE_DIR:SKRClient>/SKR/escape_menu/"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  VERBATIM)
add_dependencies(SKRClient SKRLobbyAssets)

# --- Intro timeline/transport tests (no GTA, no device) ---
add_executable(SKRLobbyTests tests/unit/test_lobby_settings.cpp client/lobby/settings_io.cpp)
target_include_directories(SKRLobbyTests PRIVATE ${CMAKE_SOURCE_DIR}/client)
target_link_libraries(SKRLobbyTests PRIVATE shell32 ole32)

add_executable(SKRIntroTests
  tests/unit/test_intro_timeline.cpp
)
target_include_directories(SKRIntroTests PRIVATE ${CMAKE_SOURCE_DIR}/client)

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

if(MSVC)
  foreach(target SKRLauncher SKRClient SKRShared SKRGate SKRAuth SKROps SKRTests SKROpsTests SKRIntroTests SKRLobbyTests SKRTestServer SKRSmokeClient SKRFakeGame)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT)
  endforeach()
endif()
