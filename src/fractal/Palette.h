#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Maps a smooth iteration count to a position along the palette.
//
// Iteration counts climb fast with zoom depth: neighbouring pixels of a deep
// view can differ by hundreds, and a position proportional to the count would
// cycle the palette several times between them, leaving colour noise. The
// square root flattens that growth. This form of it is the plain count for
// small values (its slope is 1 at zero), so shallow views keep their bands,
// and grows as 2 * sqrt(kPaletteKnee * iterations) for large ones.
//
// Must match paletteIndex() in mandelbrot_perturbation.frag.
const float kPaletteKnee = 100.0f;

inline float paletteIndex(float iterations)
{
    return 2.0f * kPaletteKnee * (std::sqrt(1.0f + iterations / kPaletteKnee) - 1.0f);
}

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------

// A palette is a closed loop of hand-picked colours, so every colour only ever
// meets the neighbours chosen for it. Between two stops the colour is
// interpolated in OKLab, where a straight line between two colours stays
// vivid instead of sagging through grey the way it does in RGB.
struct PaletteStop
{
    float position;     // in [0, 1), increasing; the loop closes from the last stop to the first
    uint32_t colour;    // sRGB, 0xRRGGBB
};

struct Palette
{
    const char* name;
    std::vector<PaletteStop> stops;
};

inline const std::vector<Palette>& palettes()
{
    static const std::vector<Palette> all = {
        // Neon: indigo through magenta, orange and yellow to teal and blue.
        { "Aurora", {
            { 0.0f / 7, 0x0d0630 }, { 1.0f / 7, 0x5a0fb0 }, { 2.0f / 7, 0xff2fa0 }, { 3.0f / 7, 0xff8c42 },
            { 4.0f / 7, 0xffe45e }, { 5.0f / 7, 0x2de2c0 }, { 6.0f / 7, 0x1b6fe0 } } },
        // Dusk: deep violet up through pink and peach to cream, and back.
        { "Sunset", {
            { 0.0f / 8, 0x140a33 }, { 1.0f / 8, 0x5b1a8b }, { 2.0f / 8, 0xc2267a }, { 3.0f / 8, 0xff5e62 },
            { 4.0f / 8, 0xffb36b }, { 5.0f / 8, 0xfff1c1 }, { 6.0f / 8, 0xffb36b }, { 7.0f / 8, 0xc2267a } } },
        // The classic deep blue, white and orange of Ultra Fractal.
        { "Ultra", {
            { 0.0f, 0x000764 }, { 0.16f, 0x206bcb }, { 0.42f, 0xedffff }, { 0.6425f, 0xffaa00 },
            { 0.8575f, 0x000200 } } },
        // Embers: near black through crimson and orange to pale yellow.
        { "Fire", {
            { 0.0f / 8, 0x05010d }, { 1.0f / 8, 0x5c0a3a }, { 2.0f / 8, 0xc72c1c }, { 3.0f / 8, 0xff8a1f },
            { 4.0f / 8, 0xffe680 }, { 5.0f / 8, 0xfff8e0 }, { 6.0f / 8, 0xff8a1f }, { 7.0f / 8, 0x7a1030 } } },
        // Sea colours with a coral accent.
        { "Lagoon", {
            { 0.0f / 8, 0x031028 }, { 1.0f / 8, 0x0b4f8a }, { 2.0f / 8, 0x19a7b8 }, { 3.0f / 8, 0x8ff0d0 },
            { 4.0f / 8, 0xfbfff0 }, { 5.0f / 8, 0xffc27a }, { 6.0f / 8, 0xe0567a }, { 7.0f / 8, 0x4a1d6e } } },
    };
    return all;
}

// The palette used when none is asked for.
const int kDefaultPalette = 0;

// The index of the palette called `name`, ignoring case, or -1.
inline int findPalette(const std::string& name)
{
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    };
    const std::vector<Palette>& all = palettes();
    for (size_t i = 0; i < all.size(); ++i) {
        if (lower(all[i].name) == lower(name)) return static_cast<int>(i);
    }
    return -1;
}

// A palette is used as a table of this many colours around its loop; lookups
// interpolate between neighbouring entries. Each entry is four floats: linear
// red, green and blue, and an unused fourth that pads it to a GLSL vec4.
//
// Must match PALETTE_TABLE_SIZE in mandelbrot_perturbation.frag.
const int kPaletteTableSize = 256;

namespace palette_detail {

struct Lab { float l, a, b; };

// sRGB 0xRRGGBB to OKLab, with Björn Ottosson's coefficients.
inline Lab toOklab(uint32_t colour)
{
    float linear[3];
    for (int channel = 0; channel < 3; ++channel) {
        const float encoded = static_cast<float>((colour >> (16 - 8 * channel)) & 0xFF) / 255.0f;
        linear[channel] = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }
    const float l = std::cbrt(0.4122214708f * linear[0] + 0.5363325363f * linear[1] + 0.0514459929f * linear[2]);
    const float m = std::cbrt(0.2119034982f * linear[0] + 0.6806995451f * linear[1] + 0.1073969566f * linear[2]);
    const float s = std::cbrt(0.0883024619f * linear[0] + 0.2817188376f * linear[1] + 0.6299787005f * linear[2]);
    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    };
}

// OKLab to linear sRGB, clamped to what the display can show.
inline void toLinearRgb(const Lab& lab, float rgb[3])
{
    const float l = lab.l + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m = lab.l - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s = lab.l - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
    const float l3 = l * l * l;
    const float m3 = m * m * m;
    const float s3 = s * s * s;
    rgb[0] = std::clamp(4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3, 0.0f, 1.0f);
    rgb[1] = std::clamp(-1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3, 0.0f, 1.0f);
    rgb[2] = std::clamp(-0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3, 0.0f, 1.0f);
}

} // namespace palette_detail

// The table for a palette: kPaletteTableSize entries of four floats.
inline std::vector<float> buildPaletteTable(const Palette& palette)
{
    using palette_detail::Lab;

    const std::vector<PaletteStop>& stops = palette.stops;
    const size_t count = stops.size();

    std::vector<float> table(static_cast<size_t>(kPaletteTableSize) * 4, 1.0f);
    for (int entry = 0; entry < kPaletteTableSize; ++entry) {
        const float position = static_cast<float>(entry) / kPaletteTableSize;

        // The last stop at or before this position; before the first stop, it
        // is the last one, reached the long way round the loop.
        size_t from = count - 1;
        for (size_t i = 0; i < count; ++i) {
            if (stops[i].position <= position) from = i;
        }
        const size_t to = (from + 1) % count;

        float start = stops[from].position;
        float end = stops[to].position;
        float at = position;
        if (end <= start) end += 1.0f;
        if (at < start) at += 1.0f;
        const float share = (end > start) ? (at - start) / (end - start) : 0.0f;

        const Lab a = palette_detail::toOklab(stops[from].colour);
        const Lab b = palette_detail::toOklab(stops[to].colour);
        const Lab mixed = { a.l + (b.l - a.l) * share, a.a + (b.a - a.a) * share, a.b + (b.b - a.b) * share };
        palette_detail::toLinearRgb(mixed, &table[static_cast<size_t>(entry) * 4]);
    }
    return table;
}

// ---------------------------------------------------------------------------
// Colouring
// ---------------------------------------------------------------------------

// Trips round the palette per unit of palette position.
//
// Must match PALETTE_FREQUENCY in mandelbrot_perturbation.frag.
const float kPaletteFrequency = 0.012f;

// The palette phase is an angle: a full turn of it carries the colours once
// round the palette, and it wraps there.
const double kPalettePhasePeriod = 2.0 * 3.141592653589793;

// The phase at a zoom of 1, before any drift.
const double kPaletteStartPhase = 0.0;

// Radians the phase advances each time the zoom doubles. Colour already moves
// outward as a view gets deeper, because it is attached to the picture; this
// makes the palette run outward through the picture as well, at a rate set by
// how fast the zoom goes rather than by the clock.
const double kPalettePhasePerDoubling = 0.5;

// The palette phase of a view. `log2Zoom` is the binary logarithm of its
// magnification, `perDoubling` how far the phase advances per doubling of the
// zoom (0 keeps the colours fixed to the picture).
inline float palettePhase(double log2Zoom, double perDoubling)
{
    return static_cast<float>(std::fmod(kPaletteStartPhase + log2Zoom * perDoubling, kPalettePhasePeriod));
}

// Contour bands: the colour is darkened over every other stretch of palette
// position, which draws hard-edged rings over the smooth gradient.
//
// Must match the PALETTE_BAND_* constants in mandelbrot_perturbation.frag.
const float kPaletteBandFrequency = 0.125f;     // light and dark pairs per unit of palette position
const float kPaletteBandDepth = 0.15f;          // how much a dark band is darkened; 0 turns the bands off
// Pairs of bands that pass a point over one turn of the phase. A whole number,
// so the bands are back where they started when the phase wraps. They move
// towards the set as the phase advances, against the colours.
const float kPaletteBandTurns = 3.0f;

// The colour of a point outside the set, as linear red, green and blue in
// [0, 1]. `table` is what buildPaletteTable() returned.
inline void paletteColour(const float* table, float iterations, float phase, float rgb[3])
{
    const float index = paletteIndex(iterations);
    const float turns = phase / static_cast<float>(kPalettePhasePeriod);

    // Where on the loop, as a table position with a fraction.
    float position = index * kPaletteFrequency + turns;
    position = (position - std::floor(position)) * kPaletteTableSize;
    const int entry = static_cast<int>(position) % kPaletteTableSize;
    const int next = (entry + 1) % kPaletteTableSize;
    const float share = position - std::floor(position);

    // Dark over the first half of each pair of bands.
    float band = index * kPaletteBandFrequency - turns * kPaletteBandTurns;
    band -= std::floor(band);
    const float shade = (band < 0.5f) ? 1.0f - kPaletteBandDepth : 1.0f;

    const float* a = table + static_cast<size_t>(entry) * 4;
    const float* b = table + static_cast<size_t>(next) * 4;
    for (int channel = 0; channel < 3; ++channel) {
        rgb[channel] = shade * (a[channel] + (b[channel] - a[channel]) * share);
    }
}
