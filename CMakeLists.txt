cmake_minimum_required(VERSION 3.16)
project(Fractasmic VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Lets editors (IntelliSense, clangd) see the real include paths and defines.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FRACTASMIC_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

if (NOT EXISTS "${FRACTASMIC_EXTERNAL_DIR}/VKE/CMakeLists.txt")
    message(FATAL_ERROR
        "Submodules are missing. Run: git submodule update --init")
endif()

# ---------------------------------------------------------------------------
# VKE
#
# Every dependency (spdlog, SDL3, Dear ImGui, stb) is a submodule under
# external/, so VKE resolves them locally and never downloads anything.
# Vulkan itself comes from the system / Vulkan SDK.
# ---------------------------------------------------------------------------
set(VKE_EXTERNAL_DIR       "${FRACTASMIC_EXTERNAL_DIR}" CACHE PATH "" FORCE)
set(VKE_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

set(VKE_WITH_GRAPHICS      ON  CACHE BOOL "" FORCE)
set(VKE_WITH_APP           ON  CACHE BOOL "" FORCE)

set(VKE_WITH_GEOMETRY      OFF CACHE BOOL "" FORCE)
set(VKE_WITH_RAYTRACING    OFF CACHE BOOL "" FORCE)
set(VKE_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)

# Build SDL as a static library alongside the app.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)

add_subdirectory("${FRACTASMIC_EXTERNAL_DIR}/VKE" VKE)

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
# Fractasmic
#
# One program: the explorer, a fullscreen pass with the perturbation fragment
# shader; keyframe mode, which renders zoom sequences offline with the same
# iteration as a resumable compute shader and browses them; and video assembly
# from the keyframes, which pipes frames to the ffmpeg program at run time, so
# ffmpeg is not a build dependency.
#
# The sources are layered; each folder uses only those listed above it:
#   src/fractal/    the maths: MPFR only, no Vulkan, no UI
#   src/keyframes/  reading and writing keyframe files
#   src/video/      blending keyframes into frames on the CPU, and encoding
#   src/ui/         ImGui pieces both modes use
#   src/modes/      the explore and keyframe modes: Vulkan and ImGui
#   src/            the app, which switches between the modes, and main()
# The shaders share shaders/perturbation_common.glsl through #include, which
# the shader build tracks as a dependency on its own.
# ---------------------------------------------------------------------------
add_executable(Fractasmic
    src/fractal/BigMath.cpp
    src/fractal/ReferenceOrbit.cpp

    src/keyframes/KeyframeSequence.cpp
    src/keyframes/KeyframeWriter.cpp

    src/video/FrameRenderer.cpp
    src/video/VideoAssembler.cpp
    src/video/VideoEncoder.cpp

    src/ui/Sidebar.cpp
    src/ui/StatusBar.cpp

    src/modes/ExploreMode.cpp
    src/modes/KeyframeMode.cpp

    src/App.cpp
    src/main.cpp
)

# src/ is the include root: "fractal/Palette.h", "ui/FontAwesome.h".
target_include_directories(Fractasmic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Video assembly runs on threads of its own.
find_package(Threads REQUIRED)
target_link_libraries(Fractasmic PRIVATE vke::vke MPFR::MPFR Threads::Threads)

# Fonts and other runtime assets are loaded from the source tree.
target_compile_definitions(Fractasmic PRIVATE "ASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/")

# Assembling a video's frames is all arithmetic on the CPU; without
# optimisation it is several times slower, and Debug builds have none.
set_source_files_properties(src/video/FrameRenderer.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-O2>")

# Compiles to <build>/shaders/*.spv and defines VKE_SHADER_DIR for the target.
vke_compile_shaders(Fractasmic
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders
    SHADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mandelbrot_perturbation.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mandelbrot_keyframe.comp
)
