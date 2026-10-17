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

# --- Launcher ---
add_executable(SKRLauncher WIN32 apps/launcher/main.cpp)
target_link_libraries(SKRLauncher PRIVATE user32)

# --- Client DLL (injected by launcher) ---
add_library(SKRClient SHARED
  client/bootstrap/dllmain.cpp
  client/skrac/mock_adapter.cpp
  client/render-d3d9/intro_renderer.cpp
  client/render-d3d9/d3d9_hook.cpp
  client/cef-bridge/cef_bridge.cpp
  client/cef-bridge/cef_plugin_exports.cpp
)
set_target_properties(SKRClient PROPERTIES
  OUTPUT_NAME "SKR.Client" PREFIX ""
)
target_include_directories(SKRClient PRIVATE
  ${CMAKE_SOURCE_DIR}/client
  ${CMAKE_SOURCE_DIR}/shared
)
target_link_libraries(SKRClient PRIVATE d3d9)

if(MSVC)
  foreach(target SKRLauncher SKRClient)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT)
  endforeach()
endif()
