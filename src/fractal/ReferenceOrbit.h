#pragma once

#include <mpfr.h>

#include <cstdint>
#include <vector>

// Must match ESCAPE_RADIUS in mandelbrot_perturbation.frag.
inline constexpr double kEscapeRadius = 256.0;

// normal float:
// | sign | exponent | mantissa |
// |  1   |    8     |    23    |  = 32 bits

// floatexp:
// | mantissa (float) |  exponent (int)  |
// |       32         |        32        | = 64 bits

// in floatexp zero is coded as mantissa 0 with ZERO_EXPONENT (−2^28) as a marker
inline constexpr int32_t kZeroExponent = -(1 << 28);

// Orbit data passed to the perturbation shader (in floatexp)
struct OrbitPoint
{
    // Complex number floatexp
    // Zₙ = (mantissa[0] + i · mantissa[1]) × 2^exponent

    float mantissa[2];
    int32_t exponent;

    // std430 aligns the struct to its vec2, which rounds its size up to 16.
    int32_t padding;
};
static_assert(sizeof(OrbitPoint) == 16, "OrbitPoint must match perturbation_common.glsl");

// Iterates Z -> Z² + C in high precision and returns the orbit rounded to
// float: Z₀ = 0, Z₁ = C, Z₂ = C² + C, ...
//
// This is the one expensive, high-precision computation of perturbation
// rendering. Every pixel then only tracks its small difference from this orbit,
// which float can represent even when the pixel's own coordinate needs far more
// digits than float has.
//
// The working precision is taken from cx. The orbit ends after `maxIters` steps
// or with the first value outside the escape radius, whichever comes first, so
// it holds at most maxIters + 1 points.
std::vector<OrbitPoint> computeReferenceOrbit(mpfr_srcptr cx, mpfr_srcptr cy, int maxIters);
