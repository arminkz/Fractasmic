#include "ReferenceOrbit.h"

#include <algorithm>
#include <cmath>

namespace {

// Rounds a high-precision point to the two float forms the shader reads.
OrbitPoint makeOrbitPoint(mpfr_srcptr x, mpfr_srcptr y)
{
    OrbitPoint point{};

    point.z[0] = mpfr_get_flt(x, MPFR_RNDN);
    point.z[1] = mpfr_get_flt(y, MPFR_RNDN);

    // Each part as d * 2^exp with |d| in [0.5, 1); zero comes back as 0 * 2^0.
    long xExponent = 0;
    long yExponent = 0;
    const double xMantissa = mpfr_get_d_2exp(&xExponent, x, MPFR_RNDN);
    const double yMantissa = mpfr_get_d_2exp(&yExponent, y, MPFR_RNDN);

    if (xMantissa == 0.0 && yMantissa == 0.0) {
        point.exponent = kZeroExponent;
        return point;
    }

    // Both parts share one exponent: that of the larger part, so its mantissa
    // stays in [0.5, 1) and the smaller one is shifted down to match.
    long exponent = 0;
    if (xMantissa == 0.0) {
        exponent = yExponent;
    } else if (yMantissa == 0.0) {
        exponent = xExponent;
    } else {
        exponent = std::max(xExponent, yExponent);
    }

    // A part more than a float's 24 bits below the other contributes nothing.
    const auto shifted = [exponent](double mantissa, long partExponent) {
        if (mantissa == 0.0 || exponent - partExponent > 64) return 0.0f;
        return static_cast<float>(std::ldexp(mantissa, static_cast<int>(partExponent - exponent)));
    };
    point.mantissa[0] = shifted(xMantissa, xExponent);
    point.mantissa[1] = shifted(yMantissa, yExponent);
    point.exponent = static_cast<int32_t>(exponent);

    return point;
}

} // namespace


mpfr_prec_t precisionForScale(mpfr_srcptr scale)
{
    // scale is below 2^exponent, so -exponent bits place a point on the screen;
    // never go below what a double already offers.
    const long positionBits = std::max(64L, -static_cast<long>(mpfr_get_exp(scale)));
    return static_cast<mpfr_prec_t>(positionBits) + 64;
}


std::vector<OrbitPoint> computeReferenceOrbit(mpfr_srcptr cx, mpfr_srcptr cy, int maxIters)
{
    const mpfr_prec_t precision = mpfr_get_prec(cx);

    mpfr_t x, y, x2, y2, xy;
    mpfr_inits2(precision, x, y, x2, y2, xy, static_cast<mpfr_ptr>(nullptr)); // C-style variadic
    mpfr_set_zero(x, 1); // +0
    mpfr_set_zero(y, 1); // +0

    std::vector<OrbitPoint> orbit;
    orbit.reserve(static_cast<size_t>(maxIters) + 1);

    // Z₀ = 0
    orbit.push_back(makeOrbitPoint(x, y));

    for (int n = 0; n < maxIters; ++n) {
        // Zₙ₊₁ = Zₙ² + C, written out in real and imaginary parts:
        // x' = x² - y² + cx,  y' = 2xy + cy
        mpfr_sqr(x2, x, MPFR_RNDN);        // ^2
        mpfr_sqr(y2, y, MPFR_RNDN);        // ^2
        mpfr_mul(xy, x, y, MPFR_RNDN);     // *

        mpfr_sub(x, x2, y2, MPFR_RNDN);    // -
        mpfr_add(x, x, cx, MPFR_RNDN);     // +

        mpfr_mul_2ui(y, xy, 1, MPFR_RNDN); // *(2^1)
        mpfr_add(y, y, cy, MPFR_RNDN);     // +

        // The shader only needs each point to float precision.
        orbit.push_back(makeOrbitPoint(x, y));

        // The escaped point is kept: a pixel that follows the reference this
        // far needs it for its own escape test.
        const double xd = mpfr_get_d(x, MPFR_RNDN);
        const double yd = mpfr_get_d(y, MPFR_RNDN);
        if (xd * xd + yd * yd > kEscapeRadius * kEscapeRadius) break;
    }

    mpfr_clears(x, y, x2, y2, xy, static_cast<mpfr_ptr>(nullptr));

    return orbit;
}
