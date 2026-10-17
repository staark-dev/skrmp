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

# --- Shared library (protocol, crypto, updater) ---
add_library(SKRShared STATIC
  shared/crypto/crypto.cpp
  shared/updater/manifest.cpp
  shared/updater/release_gate.cpp
)
target_include_directories(SKRShared PUBLIC ${CMAKE_SOURCE_DIR}/shared)
target_link_libraries(SKRShared PRIVATE bcrypt)

# --- Launcher (with updater + inventory) ---
add_executable(SKRLauncher WIN32
  apps/launcher/main.cpp
  apps/launcher/updater.cpp
  apps/launcher/inventory.cpp
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
  client/render-d3d9/intro_renderer.cpp
  client/render-d3d9/d3d9_hook.cpp
  client/cef-bridge/cef_bridge.cpp
  client/cef-bridge/cef_plugin_exports.cpp
  client/compatibility/r2_adapter.cpp
  client/compatibility/compat_profile.cpp
  client/transport/auth_client.cpp
)
set_target_properties(SKRClient PROPERTIES
  OUTPUT_NAME "SKR.Client" PREFIX ""
)
target_include_directories(SKRClient PRIVATE
  ${CMAKE_SOURCE_DIR}/client
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRClient PRIVATE d3d9 SKRShared)

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
target_link_libraries(SKRTests PRIVATE SKRShared bcrypt)

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

if(MSVC)
  foreach(target SKRLauncher SKRClient SKRShared SKRGate SKRAuth SKROps SKRTests SKROpsTests SKRTestServer SKRSmokeClient)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT)
  endforeach()
endif()
