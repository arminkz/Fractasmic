# Fractasmic

A Vulkan fractal explorer built on [VulkanEngine](https://github.com/arminkz/VulkanEngine).

It builds two executables:

| Executable | What it does |
| --- | --- |
| `FloatFractalRenderer` | Mandelbrot set in single precision, every pixel iterated directly. |
| `DeepZoomFractalRenderer` | Mandelbrot set by perturbation, for zoom far beyond float precision. |

## Zoom depth

How far each technique can zoom before the image breaks down:

| Technique | Executable | Max zoom | What sets the limit |
| --- | --- | --- | --- |
| Naive float | `FloatFractalRenderer` | about 5 × 10⁴ | A float has about 7 significant digits, so neighbouring pixels get the same coordinate. |
| Perturbation, MPFR reference orbit with float deltas | `DeepZoomFractalRenderer`, down to about 10²⁴ | about 10³⁰ | The per-pixel offsets from the reference orbit fall below the smallest value a float can hold (about 10⁻³⁸). |
| Perturbation, MPFR reference orbit with extended-exponent deltas | `DeepZoomFractalRenderer`, past about 10²⁴ | 10¹⁰⁰⁰ | A clamp in the renderer, not the technique: each delta is a float mantissa with a separate integer exponent, which has no floor. The clamp bounds the cost of the reference orbit. |

Zoom is the magnification relative to a view whose half-height is 1 in the complex plane. Both renderers clamp at their limits.

`DeepZoomFractalRenderer` switches between the two perturbation variants on its own: plain float deltas while they are safely inside float's range, because they are faster, and extended-exponent deltas beyond that.

In both variants the MPFR precision of the reference orbit grows with the zoom, so the orbit is not what limits depth. What deep views do need is more iterations, and the renderer caps those at 16,384.

## Requirements

These must be installed on your system before building:

- **Vulkan SDK**, including `glslc` for compiling shaders
- **MPFR** and **GMP**, for the arbitrary-precision arithmetic behind deep zoom
- **CMake** 3.16 or newer
- A **C++17** compiler

On Ubuntu or Debian, MPFR and GMP come from:

```bash
sudo apt install libmpfr-dev libgmp-dev
```

Install the Vulkan SDK from [LunarG](https://vulkan.lunarg.com/sdk/home), or use your distribution's Vulkan and shaderc packages.

Everything else is bundled as a git submodule under `external/` and needs no installation: VulkanEngine, SDL3, Dear ImGui, spdlog and stb.

## Building

Clone with submodules:

```bash
git clone --recursive git@github.com:arminkz/Fractasmic.git
cd Fractasmic
```

If you already cloned without `--recursive`:

```bash
git submodule update --init
```

Then configure and build:

```bash
cmake -S . -B build
cmake --build build -j
```

## Running

```bash
./build/FloatFractalRenderer
./build/DeepZoomFractalRenderer
```

Fonts are loaded from the `assets/` folder of the source tree, so keep the source tree where it was when you built.

## Controls

- **Drag** with the left mouse button to pan
- **Scroll** to zoom about the cursor
- The **Fractasmic** panel sets iterations, anti-aliasing and palette animation, and resets the view

## License

GPL-3.0. See [LICENSE](LICENSE).
