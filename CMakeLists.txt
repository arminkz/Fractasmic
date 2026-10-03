cmake_minimum_required(VERSION 3.16)
project(Fractasmic VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Lets editors (IntelliSense, clangd) see the real include paths and defines.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FRACTASMIC_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

if (NOT EXISTS "${FRACTASMIC_EXTERNAL_DIR}/VulkanEngine/CMakeLists.txt")
    message(FATAL_ERROR
        "Submodules are missing. Run: git submodule update --init")
endif()

# ---------------------------------------------------------------------------
# VulkanEngine
#
# Every dependency (spdlog, SDL3, Dear ImGui, stb) is a submodule under
# external/, so VulkanEngine resolves them locally and never downloads anything.
# Vulkan itself comes from the system / Vulkan SDK.
# ---------------------------------------------------------------------------
set(VKE_EXTERNAL_DIR       "${FRACTASMIC_EXTERNAL_DIR}" CACHE PATH "" FORCE)
set(VKE_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

set(VKE_WITH_GRAPHICS      ON  CACHE BOOL "" FORCE)
set(VKE_WITH_APP           ON  CACHE BOOL "" FORCE)
# Meshes, OBJ loading and cameras would pull in glm and tinyobjloader.
set(VKE_WITH_GEOMETRY      OFF CACHE BOOL "" FORCE)
set(VKE_WITH_RAYTRACING    OFF CACHE BOOL "" FORCE)
set(VKE_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)

# Build SDL as a static library alongside the app.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)

add_subdirectory("${FRACTASMIC_EXTERNAL_DIR}/VulkanEngine" VulkanEngine)

# ---------------------------------------------------------------------------
# Fractasmic
# ---------------------------------------------------------------------------
add_executable(Fractasmic
    src/main.cpp
    src/FractalRenderer.cpp
)

target_link_libraries(Fractasmic PRIVATE vke::vke)

# Fonts and other runtime assets are loaded from the source tree.
target_compile_definitions(Fractasmic PRIVATE "ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/")

# Compiles to <build>/shaders/*.spv and defines VKE_SHADER_DIR for the target.
vke_compile_shaders(Fractasmic
    SHADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mandelbrot.frag
)
