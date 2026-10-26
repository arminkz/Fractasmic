# Fractasmic

A Vulkan fractal explorer built on [VKE](https://github.com/arminkz/VKE).

It renders the Mandelbrot set by perturbation, for zoom far beyond float precision. One program, `Fractasmic`, does it all:

- **Explore** the set in real time: pan, zoom and rotate with the mouse.
- **Generate keyframes** of a zoom offline, as raw iteration data, in its [keyframe mode](#keyframe-mode).
- **Assemble a video** from them: colour the keyframes, interpolate between them and encode the zoom as a video file.

A deep-zoom video goes through all three: find a location, generate its keyframes, then [assemble the video](#assembling-a-video).

## Zoom depth

How far each technique can zoom before the image breaks down:

| Technique | Max zoom | What sets the limit |
| --- | --- | --- |
| Perturbation, MPFR reference orbit with float deltas | about 10³⁰ | The per-pixel offsets from the reference orbit fall below the smallest value a float can hold (about 10⁻³⁸). |
| Perturbation, MPFR reference orbit with extended-exponent deltas | 10¹⁰⁰⁰ | A clamp in the renderer, not the technique: each delta is a float mantissa with a separate integer exponent, which has no floor. The clamp bounds the cost of the reference orbit. |

Zoom is the magnification relative to a view whose half-height is 1 in the complex plane.

The renderers use extended-exponent deltas at every depth. At shallow zooms plain float deltas would be faster, with the same precision, but a single variant keeps one code path for the whole range.

In both variants the MPFR precision of the reference orbit grows with the zoom, so the orbit is not what limits depth. What deep views do need is more iterations. The explorer caps those at 16,384 to stay interactive; keyframe mode goes up to 1,000,000 by default.

## Iterations

How many iterations a view needs depends more on where it is than on how deep, so both the explorer and the keyframes choose the limit automatically. The signal is the pixels that escape just under the limit: if more than 0.5% of them escape in its top half, between half the limit and the limit, more would escape above it, and the limit doubles. Pixels inside the set never escape, so they do not drive it up.

- **The explorer** counts where the pixels of each frame escaped, from one pixel in 16, and doubles or halves the limit from frame to frame. Turn off **Automatic iterations** in the panel to set it by hand.
- **Keyframes** each get their own limit. A keyframe is first rendered small, starting from half the previous keyframe's limit and doubling until the rule is met; then it is rendered in full with that limit. `keyframes.json` records each keyframe's limit.

Near the boundary the share of late pixels often only halves with each doubling, so a fraction of a percent of the pixels may come out black that a much higher limit would colour. A pixel that needs far more than the limit also looks the same as one inside the set, which is why the search always starts close to a limit that suited a neighbouring frame or keyframe.

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

Assembling videos also needs **ffmpeg** on the `PATH` when it runs (`sudo apt install ffmpeg`). It is not needed to build.

Everything else is bundled as a git submodule under `external/` and needs no installation: VKE, SDL3, Dear ImGui, spdlog and stb.

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

Fonts are loaded from the `assets/` folder of the source tree, so keep the source tree where it was when you built.

## Exploring

```bash
./build/Fractasmic
```

- **Drag** with the left mouse button to pan
- **Scroll** to zoom about the cursor
- The **Fractasmic** panel sets anti-aliasing, the palette and whether its colours cycle with the zoom, and rotation, resets the view, and sets the [iterations](#iterations) by hand if automatic iterations are off
- The fractal is redrawn only when the view, the iterations, the palette or the window size change; otherwise the last image is shown again. **Last render** in the panel is how long that redraw took on the GPU
- The status bar at the bottom shows the zoom and the centre
- **Copy location** in the panel puts the current view on the clipboard as [command-line arguments](#command-line) that generate its keyframes

## Keyframe mode

Renders a zoom sequence towards one point. The first keyframe shows the whole set, each next one is twice as deep, and the last is the zoom you asked for: about 3.3 keyframes per power of ten.

From the explorer's **File** menu:

- **Generate Keyframes...** asks for the centre, the zoom and the folder to save to, filled in with the current view, then generates the sequence. **Paste location** in the dialog fills it from what **Copy location** put on the clipboard.
- **Open Keyframes...** picks the folder of a sequence generated earlier and shows its keyframes.

Either switches the window to keyframe mode. Its own **File** menu starts or opens another sequence, and **Stop generating** ends a run early; the finished keyframes stay on disk. **Back to explorer**, in the panel or the menu, returns to the view you left. It stops a generation in progress.

While a sequence is generating, the window shows the last finished keyframe and the progress. Afterwards, and for an opened sequence, the left and right arrow keys step through the keyframes (Home and End jump to the first and last), as do the buttons and slider in the panel.

Keyframes already present in the output folder are skipped, so an interrupted run can be resumed by starting the same sequence again. A folder that holds keyframes of a different sequence is refused.

### Command line

A sequence can also be generated from the command line, for example in a script. The window then opens in keyframe mode:

```bash
./build/Fractasmic --re -0.745 --im 0.186 --zoom 1e12
```

| Argument | Meaning | Default |
| --- | --- | --- |
| `--re <decimal>` | Real part of the centre | |
| `--im <decimal>` | Imaginary part of the centre | |
| `--zoom <number>` | Final magnification, for example `1e50` | |
| `--max-iterations <n>` | The most [iterations](#iterations) a keyframe may be given | 1000000 |
| `--iterations <n>` | Use `n` iterations for every keyframe instead of choosing them | |
| `--width <pixels>` | Keyframe width | 1920 |
| `--height <pixels>` | Keyframe height | 1080 |
| `--output <folder>` | Where the keyframes are written | `keyframes` |
| `--exit-when-done` | Close the window after the last keyframe | off |
| `--open <folder>` | Browse the keyframes in this folder instead of generating | |

With `--re`, `--im` and `--zoom` the sequence is generated straight away. Without them, the explorer opens and the other options only pre-fill the **Generate Keyframes** dialog.

### Output

- `keyframe_00000.pfm`, `keyframe_00001.pfm`, …: one greyscale [PFM](https://www.pauldebevec.com/Research/HDR/PFM/) image per keyframe, 32-bit float per pixel. Each value is the pixel's smooth iteration count; 0 means the point is inside the set.
- `keyframes.json`: the centre and size, and the zoom and iteration limit of every keyframe.

The keyframes carry no colour. Colouring, and interpolating between keyframes, happens when the video is assembled, so the palette can change without re-rendering.

Keyframes are not anti-aliased, because averaging iteration counts is wrong at the set's boundary. Render them larger than the video (for example twice the size) and downsample after colouring.

The iteration runs in bounded steps on the GPU, so a very high iteration limit makes a render slow but does not stall the GPU.

## Assembling a video

While keyframe mode shows a finished or opened sequence, **Assemble Video...**, in the panel or the **File** menu, turns it into a video. The dialog asks for:

| Field | Meaning | Default |
| --- | --- | --- |
| Save to | The video file; its extension, such as `.mp4`, picks the container | the keyframe folder's name with `.mp4` |
| Frames per second | | 60 |
| Seconds per keyframe | How long the zoom takes to double | 1.0 |
| Width | The video's width; the height follows the keyframes' aspect ratio. **Half** sets half the keyframes' width | the keyframes' width |
| Palette | The colours, as in the explorer | the one keyframe mode shows |
| Colours follow the zoom | Cycle the colours as the zoom deepens, as the explorer's **Follow zoom** does | on |
| Codec | H.264 plays almost everywhere; H.265 makes smaller files | H.264 |
| Quality | 0 to 51; lower is better and larger | 18 |
| Encoder | GPU encodes with NVIDIA NVENC, CPU with libx264/libx265 | GPU |

It also shows the video's size, frame count and length before you start. The video is assembled in the background while the panel shows its progress; until it ends or you **Cancel** it, keyframe mode stays on this sequence. A cancelled video's unfinished file is deleted, and if ffmpeg fails, its own message is shown.

The video starts on the first keyframe and ends on the last, so its length is the number of keyframes times the seconds per keyframe.

Each frame is built from the two keyframes around it: the deeper one fills the middle of the frame and the wider one the ring around it. Colour uses the same palette as the explorer and is applied before any filtering.

For the sharpest result, render the keyframes at twice the video's size, for example 3840x2160 for a 1920x1080 video, and press **Half**. With keyframes the same size as the video, the outer ring of each frame is enlarged by up to 2x and looks softer.

Fractal detail compresses poorly, so files are large at the default quality. Raise the quality number (for example to 24) or use H.265 for smaller files.

The GPU encoder needs an NVIDIA card and an ffmpeg built with NVENC. Choose the CPU encoder on other machines, or for smaller files at the same quality in exchange for a slower encode.

## License

GPL-3.0. See [LICENSE](LICENSE).
