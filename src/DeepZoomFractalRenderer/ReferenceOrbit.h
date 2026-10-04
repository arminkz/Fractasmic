#pragma once

#include <mpfr.h>

#include <cstdint>
#include <vector>

// Must match ESCAPE_RADIUS in mandelbrot_perturbation.frag.
inline constexpr double kEscapeRadius = 256.0;

// Exponent that stands for zero in the mantissa/exponent form below.
// Must match ZERO_EXPONENT in mandelbrot_perturbation.frag.
inline constexpr int32_t kZeroExponent = -(1 << 28);

// One point of the reference orbit, in the two forms the shader reads.
// Matches OrbitPoint in mandelbrot_perturbation.frag (std430).
struct OrbitPoint
{
    // Plain float value, for the float path. Underflows to zero when the orbit
    // passes closer to the origin than a float can express.
    float z[2];

    // The same value as mantissa * 2^exponent, with the larger mantissa
    // component in [0.5, 1), so no value is too small to represent.
    float mantissa[2];
    int32_t exponent;

    int32_t padding;
};
static_assert(sizeof(OrbitPoint) == 24, "OrbitPoint must match mandelbrot_perturbation.frag");

// Bits of precision the centre coordinate and the reference orbit need at a
// given scale (half the screen height, in complex-plane units): enough to
// resolve a pixel, plus headroom for the rounding that accumulates over the
// iterations.
mpfr_prec_t precisionForScale(mpfr_srcptr scale);

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
