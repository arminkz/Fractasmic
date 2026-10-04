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
# MPFR and GMP
#
# System libraries, like Vulkan: both build with autotools, so they cannot be
# pulled in with add_subdirectory() the way the submodules are. Neither ships a
# CMake package, hence the manual search.
# ---------------------------------------------------------------------------
find_path(GMP_INCLUDE_DIR gmp.h)
find_library(GMP_LIBRARY gmp)
find_path(MPFR_INCLUDE_DIR mpfr.h)
find_library(MPFR_LIBRARY mpfr)

if (NOT GMP_INCLUDE_DIR OR NOT GMP_LIBRARY)
    message(FATAL_ERROR "GMP not found. On Ubuntu/Debian: sudo apt install libgmp-dev")
endif()
if (NOT MPFR_INCLUDE_DIR OR NOT MPFR_LIBRARY)
    message(FATAL_ERROR "MPFR not found. On Ubuntu/Debian: sudo apt install libmpfr-dev")
endif()

add_library(GMP::GMP UNKNOWN IMPORTED)
set_target_properties(GMP::GMP PROPERTIES
    IMPORTED_LOCATION "${GMP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}"
)

# MPFR is built on GMP, so linking it brings GMP along.
add_library(MPFR::MPFR UNKNOWN IMPORTED)
set_target_properties(MPFR::MPFR PROPERTIES
    IMPORTED_LOCATION "${MPFR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MPFR_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES GMP::GMP
)

# ---------------------------------------------------------------------------
# FloatFractalRenderer — Mandelbrot in single precision
#
# Each renderer is its own executable with its sources under src/<name>/.
# ---------------------------------------------------------------------------
add_executable(FloatFractalRenderer
    src/FloatFractalRenderer/main.cpp
    src/FloatFractalRenderer/FloatFractalRenderer.cpp
)

# src/ holds headers shared between the executables (FontAwesome.h).
target_include_directories(FloatFractalRenderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(FloatFractalRenderer PRIVATE vke::vke)

# Fonts and other runtime assets are loaded from the source tree.
target_compile_definitions(FloatFractalRenderer PRIVATE "ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/")

# Compiles to <build>/shaders/FloatFractalRenderer/*.spv and defines
# VKE_SHADER_DIR for the target. The per-target directory keeps executables
# that share a shader source from writing the same output file.
vke_compile_shaders(FloatFractalRenderer
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders/FloatFractalRenderer
    SHADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mandelbrot.frag
)

# ---------------------------------------------------------------------------
# DeepZoomFractalRenderer — Mandelbrot beyond float precision
# ---------------------------------------------------------------------------
add_executable(DeepZoomFractalRenderer
    src/DeepZoomFractalRenderer/main.cpp
    src/DeepZoomFractalRenderer/DeepZoomFractalRenderer.cpp
    src/DeepZoomFractalRenderer/ReferenceOrbit.cpp
)

target_include_directories(DeepZoomFractalRenderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(DeepZoomFractalRenderer PRIVATE vke::vke MPFR::MPFR)
target_compile_definitions(DeepZoomFractalRenderer PRIVATE "ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/")

vke_compile_shaders(DeepZoomFractalRenderer
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders/DeepZoomFractalRenderer
    SHADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mandelbrot_perturbation.frag
)
