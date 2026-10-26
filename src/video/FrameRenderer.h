#pragma once

#include "fractal/Palette.h"
#include "keyframes/KeyframeSequence.h"

#include <array>
#include <cstdint>
#include <vector>

// Turns keyframes into finished video frames: picks the right keyframe for
// each pixel, colours the iteration counts and filters the result.
//
// A frame sits between two keyframes. `outer` is the keyframe at or just wider
// than the frame, magnified by 2^fraction to fill it. `inner` is the next,
// twice as deep, which covers the middle of the frame at twice the detail, so
// it is used wherever it reaches and `outer` only for the ring around it.
//
// Colour is applied before any averaging, never after: iteration counts jump
// at the set's boundary, and a blend of two counts is a third, unrelated
// colour.
class FrameRenderer
{
public:
    FrameRenderer(uint32_t width, uint32_t height, const Palette& palette, unsigned threadCount);

    // Renders one frame as tightly packed 8-bit RGB, top row first.
    // `fraction` is in [0, 1): how far the zoom is from `outer` to `inner`.
    // `inner` may be null for the final frame, where fraction is 0.
    // `palettePhase` shifts the palette, as following the zoom does in the explorer.
    const std::vector<uint8_t>& render(const Keyframe& outer, 
                                       const Keyframe* inner,
                                       double fraction, float palettePhase);

private:
    uint32_t _width;
    uint32_t _height;
    unsigned _threadCount;
    std::vector<uint8_t> _rgb;

    // The palette's colour table, as buildPaletteTable() returns it.
    std::vector<float> _palette;

    // The linear-to-sRGB transfer function, as a table.
    static constexpr int kEncodeSize = 4096;
    std::array<uint8_t, kEncodeSize + 1> _encode;

    struct Colour { float r, g, b; };

    // The explorer's colouring: black inside the set, the palette outside.
    Colour colourOf(float iterations, float palettePhase) const;

    void renderRows(uint32_t firstRow, 
                    uint32_t lastRow, 
                    const Keyframe& outer, 
                    const Keyframe* inner,
                    double fraction, float palettePhase);
};
