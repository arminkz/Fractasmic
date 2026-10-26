#version 450
#extension GL_GOOGLE_include_directive : require

// Mandelbrot by perturbation: the reference orbit Zₙ of the screen centre is
// computed on the CPU in high precision, and each pixel only iterates its small
// difference δzₙ from that orbit.
//
// At depth the differences fall below the smallest float (about 1e-38), so they
// are carried as a float mantissa with a separate integer exponent, which has
// no such floor. The same form is used at every depth, so one loop covers all.

// The orbit layout, the constants and the extended-exponent arithmetic, shared
// with mandelbrot_keyframe.comp.
#include "perturbation_common.glsl"

// Matches ExploreMode::PushConstants.
layout(push_constant) uniform PushConstants {
    vec2 resolution;        // framebuffer size in pixels
    vec2 rotation;          // camera rotation as a unit complex number (cos θ, sin θ)
    // Half the screen height in complex-plane units, as mantissa * 2^exponent.
    float scaleMantissa;
    int scaleExponent;
    float palettePhase;
    int maxIters;
    int orbitLength;        // number of points in the reference orbit
} pc;

// Reference orbit of the screen centre C: Z₀ = 0, Z₁ = C, Z₂ = C² + C, ...
layout(std430, set = 0, binding = 0) readonly buffer ReferenceOrbit {
    OrbitPoint points[];
} orbit;

// Escape statistics for choosing the iteration limit, gathered from one pixel
// in 16. escapes[k] counts the samples that escaped at an iteration n with
// floor(log2(n)) == k, n = 0 counting as 1; inside counts the samples that
// never escaped. Matches ExploreMode::EscapeStats.
layout(std430, set = 0, binding = 2) buffer EscapeStats {
    uint escapes[32];
    uint inside;
} stats;

layout(location = 0) out vec4 outColor;

// Maps a smooth iteration count to a position along the palette: the plain
// count for small values, growing only as its square root for large ones.
// Iteration counts climb fast with zoom depth, and a position proportional to
// them would cycle the palette several times between neighbouring pixels.
//
// Must match paletteIndex() in src/fractal/Palette.h.
const float PALETTE_KNEE = 100.0;

float paletteIndex(float iterations) {
    return 2.0 * PALETTE_KNEE * (sqrt(1.0 + iterations / PALETTE_KNEE) - 1.0);
}

// The palette is a closed loop of colours, uploaded as a table by the CPU
// (buildPaletteTable() in src/fractal/Palette.h). Entries are linear RGB; the
// fourth component is unused.
const int PALETTE_TABLE_SIZE = 256;

layout(std430, set = 0, binding = 1) readonly buffer PaletteTable {
    vec4 colours[PALETTE_TABLE_SIZE];
} paletteTable;

// Trips round the palette per unit of palette position, and how many turns of
// the phase carry the colours once round it.
//
// Must match kPaletteFrequency and kPalettePhasePeriod in src/fractal/Palette.h.
const float PALETTE_FREQUENCY = 0.012;
const float PALETTE_PHASE_PERIOD = 2.0 * 3.141592653589793;

// Contour bands: the colour is darkened over every other stretch of palette
// position, which draws hard-edged rings over the smooth gradient. They move
// towards the set as the phase advances, a whole number of pairs per turn of
// the phase so nothing jumps when it wraps.
//
// Must match the kPaletteBand* constants in src/fractal/Palette.h.
const float PALETTE_BAND_FREQUENCY = 0.125;
const float PALETTE_BAND_DEPTH = 0.15;
const float PALETTE_BAND_TURNS = 3.0;

// Matches paletteColour() in src/fractal/Palette.h.
vec3 palette(float iterations) {
    float index = paletteIndex(iterations);
    float turns = pc.palettePhase / PALETTE_PHASE_PERIOD;

    // Where on the loop, as a table position with a fraction.
    float position = fract(index * PALETTE_FREQUENCY + turns) * float(PALETTE_TABLE_SIZE);
    int entry = int(position) % PALETTE_TABLE_SIZE;
    int next = (entry + 1) % PALETTE_TABLE_SIZE;
    vec3 colour = mix(paletteTable.colours[entry].rgb, paletteTable.colours[next].rgb, fract(position));

    // Dark over the first half of each pair of bands.
    float band = fract(index * PALETTE_BAND_FREQUENCY - turns * PALETTE_BAND_TURNS);
    return colour * (1.0 - PALETTE_BAND_DEPTH * (1.0 - step(0.5, band)));
}

float smoothIterations(int n, vec2 z) {
    if (n >= pc.maxIters) return 0.0;

    return float(n) - log2(log2(dot(z, z))) + 4.0;
}

// Counts where a sample stopped, for one pixel in 16: enough for the
// statistics, and few enough atomics not to slow the frame down.
void recordEscape(int n) {
    if (any(notEqual(ivec2(gl_FragCoord.xy) & 3, ivec2(0)))) return;

    if (n >= pc.maxIters) {
        atomicAdd(stats.inside, 1u);
    } else {
        atomicAdd(stats.escapes[findMSB(uint(max(n, 1)))], 1u);
    }
}

ExtComplex orbitPoint(int index) {
    return ExtComplex(orbit.points[index].mantissa, orbit.points[index].exponent);
}

// Smooth iteration count, or 0 for points inside the set.
// dc is the pixel's offset from the reference point C.
float mandelbrot(ExtComplex dc) {
    ExtComplex dz = ExtComplex(vec2(0.0), ZERO_EXPONENT);   // δzₙ, the offset from the reference orbit
    vec2 z = vec2(0.0);     // zₙ as a plain float, only needed once it gets large
    int ref = 0;            // index into the reference orbit
    int n = 0;
    for (; n < pc.maxIters; n++) {
        // δzₙ₊₁ = 2·Zₙ·δzₙ + δzₙ² + δc
        ExtComplex Z = orbitPoint(ref);
        ExtComplex twoZ = ExtComplex(Z.m, Z.e + 1);
        dz = extAdd(extMul(extAdd(twoZ, dz), dz), dc);
        ref++;

        // zₙ = Zₙ + δzₙ
        ExtComplex zExt = extAdd(orbitPoint(ref), dz);

        // A value past the escape radius has an exponent of at least 8, so the
        // conversion to float is only done, and only safe, from there up.
        if (zExt.e >= 8) {
            z = zExt.m * powerOfTwo(min(zExt.e, 60));
            if (dot(z, z) > ESCAPE_RADIUS * ESCAPE_RADIUS) break;
        }

        // Rebase: when z is smaller than δz, the sum above has cancelled away
        // its significant digits. Carry z on as the new δz against the start of
        // the reference orbit (Z₀ = 0), where it is exact again. The same move
        // handles running off the end of the reference orbit.
        if (extLess(zExt, dz) || ref == pc.orbitLength - 1) {
            dz = zExt;
            ref = 0;
        }
    }

    recordEscape(n);
    return smoothIterations(n, z);
}

void main() {

    vec2 uv = (2.0 * gl_FragCoord.xy - pc.resolution) / pc.resolution.y;

    // Vulkan's y points down; flip it so the imaginary axis points up.
    uv.y = -uv.y;

    // Offset from the screen centre, which is where the reference orbit starts,
    // still in screen units. Multiplying by a unit complex number rotates the
    // view about that centre.
    vec2 offset = complexMul(uv, pc.rotation);

    // δc = offset * scale.
    float l = mandelbrot(extNormalize(offset * pc.scaleMantissa, pc.scaleExponent));

    // select color based on mandelbrot's output
    vec3 col = (l < 0.5) ? vec3(0.0) : palette(l);

    outColor = vec4(col, 1.0);
}
