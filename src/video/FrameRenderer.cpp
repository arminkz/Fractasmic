#include "FrameRenderer.h"

#include "fractal/Palette.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

// Most samples per axis taken for one output pixel when a keyframe is being
// shrunk. Shrinking never exceeds 4x in normal use (an inner keyframe twice the
// video's size, at the start of its range).
const int kMaxTapsPerAxis = 8;

} // namespace


FrameRenderer::FrameRenderer(uint32_t width, uint32_t height, const Palette& palette, unsigned threadCount)
    : _width(width), _height(height), _threadCount(std::max(1u, threadCount)),
      _rgb(static_cast<size_t>(width) * height * 3), _palette(buildPaletteTable(palette))
{
    // The palette values are treated as linear light, as they are when the
    // explorer writes them to its sRGB swapchain, so the video matches what
    // the explorer shows.
    for (int i = 0; i <= kEncodeSize; ++i) {
        const float linear = static_cast<float>(i) / kEncodeSize;
        const float encoded = linear <= 0.0031308f ? 12.92f * linear
                                                   : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        _encode[i] = static_cast<uint8_t>(std::clamp(encoded, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}


FrameRenderer::Colour FrameRenderer::colourOf(float iterations, float palettePhase) const
{
    if (iterations < 0.5f) return { 0.0f, 0.0f, 0.0f };

    float rgb[3];
    paletteColour(_palette.data(), iterations, palettePhase, rgb);
    return { rgb[0], rgb[1], rgb[2] };
}


const std::vector<uint8_t>& FrameRenderer::render(const Keyframe& outer, 
                                                  const Keyframe* inner,
                                                  double fraction, float palettePhase)
{
    // Rows are independent, so split them evenly across the threads.
    std::vector<std::thread> threads;
    threads.reserve(_threadCount);
    for (unsigned i = 0; i < _threadCount; ++i) {
        const uint32_t firstRow = static_cast<uint32_t>(static_cast<uint64_t>(_height) * i / _threadCount);
        const uint32_t lastRow = static_cast<uint32_t>(static_cast<uint64_t>(_height) * (i + 1) / _threadCount);
        threads.emplace_back([=, &outer] {
            renderRows(firstRow, lastRow, outer, inner, fraction, palettePhase);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    return _rgb;
}


void FrameRenderer::renderRows(uint32_t firstRow, 
                               uint32_t lastRow, 
                               const Keyframe& outer, 
                               const Keyframe* inner,
                               double fraction, float palettePhase)
{
    const float frameWidth = static_cast<float>(_width);
    const float frameHeight = static_cast<float>(_height);

    // How much each keyframe is magnified to fill the frame. The outer one
    // grows from 1x to 2x as the zoom moves on; the inner one, twice as deep,
    // from 0.5x to 1x.
    const float outerMagnification = static_cast<float>(std::pow(2.0, fraction));
    const float innerMagnification = outerMagnification * 0.5f;

    // The inner keyframe covers the frame's middle. Stop a pixel short of its
    // edge so no sample reaches past it.
    const float innerHalfHeight = inner ? innerMagnification * (1.0f - 2.0f / static_cast<float>(inner->height)) : 0.0f;
    const float innerHalfWidth = innerHalfHeight * frameWidth / frameHeight;

    for (uint32_t y = firstRow; y < lastRow; ++y) {
        uint8_t* out = &_rgb[static_cast<size_t>(y) * _width * 3];

        for (uint32_t x = 0; x < _width; ++x, out += 3) {
            // Pixel centre relative to the frame centre, in half-heights: the
            // same uv the shaders use, with y pointing down.
            const float u = (2.0f * (static_cast<float>(x) + 0.5f) - frameWidth) / frameHeight;
            const float v = (2.0f * (static_cast<float>(y) + 0.5f) - frameHeight) / frameHeight;

            const bool useInner = inner && std::fabs(u) < innerHalfWidth && std::fabs(v) < innerHalfHeight;
            const Keyframe& source = useInner ? *inner : outer;
            const float magnification = useInner ? innerMagnification : outerMagnification;

            const float sourceWidth = static_cast<float>(source.width);
            const float sourceHeight = static_cast<float>(source.height);
            const int maxX = static_cast<int>(source.width) - 1;
            const int maxY = static_cast<int>(source.height) - 1;

            // Size of this output pixel measured in the keyframe's pixels, and
            // where its centre falls there.
            const float footprint = sourceHeight / (frameHeight * magnification);
            const float centreX = (u / magnification * sourceHeight + sourceWidth) * 0.5f;
            const float centreY = (v / magnification * sourceHeight + sourceHeight) * 0.5f;

            Colour sum{ 0.0f, 0.0f, 0.0f };
            float weight = 0.0f;

            if (footprint >= 1.0f) {
                // Shrinking: several keyframe pixels land in this one. Average
                // a grid of them, each coloured on its own.
                const int taps = std::min(kMaxTapsPerAxis, static_cast<int>(std::ceil(footprint)));
                for (int j = 0; j < taps; ++j) {
                    const float sampleY = centreY + ((static_cast<float>(j) + 0.5f) / static_cast<float>(taps) - 0.5f) * footprint;
                    const int pixelY = std::clamp(static_cast<int>(std::floor(sampleY)), 0, maxY);
                    for (int i = 0; i < taps; ++i) {
                        const float sampleX = centreX + ((static_cast<float>(i) + 0.5f) / static_cast<float>(taps) - 0.5f) * footprint;
                        const int pixelX = std::clamp(static_cast<int>(std::floor(sampleX)), 0, maxX);
                        const Colour c = colourOf(source.at(static_cast<uint32_t>(pixelX), static_cast<uint32_t>(pixelY)), palettePhase);
                        sum.r += c.r;
                        sum.g += c.g;
                        sum.b += c.b;
                    }
                }
                weight = static_cast<float>(taps * taps);
            } else {
                // Enlarging: blend the colours of the four keyframe pixels
                // around the sample point.
                const float sampleX = centreX - 0.5f;
                const float sampleY = centreY - 0.5f;
                const float floorX = std::floor(sampleX);
                const float floorY = std::floor(sampleY);
                const float blendX = sampleX - floorX;
                const float blendY = sampleY - floorY;
                const int x0 = std::clamp(static_cast<int>(floorX), 0, maxX);
                const int y0 = std::clamp(static_cast<int>(floorY), 0, maxY);
                const int x1 = std::min(x0 + 1, maxX);
                const int y1 = std::min(y0 + 1, maxY);

                const auto add = [&](int pixelX, int pixelY, float w) {
                    const Colour c = colourOf(source.at(static_cast<uint32_t>(pixelX), static_cast<uint32_t>(pixelY)), palettePhase);
                    sum.r += c.r * w;
                    sum.g += c.g * w;
                    sum.b += c.b * w;
                };
                add(x0, y0, (1.0f - blendX) * (1.0f - blendY));
                add(x1, y0, blendX * (1.0f - blendY));
                add(x0, y1, (1.0f - blendX) * blendY);
                add(x1, y1, blendX * blendY);
                weight = 1.0f;
            }

            const float scale = static_cast<float>(kEncodeSize) / weight;
            out[0] = _encode[std::clamp(static_cast<int>(sum.r * scale + 0.5f), 0, kEncodeSize)];
            out[1] = _encode[std::clamp(static_cast<int>(sum.g * scale + 0.5f), 0, kEncodeSize)];
            out[2] = _encode[std::clamp(static_cast<int>(sum.b * scale + 0.5f), 0, kEncodeSize)];
        }
    }
}
