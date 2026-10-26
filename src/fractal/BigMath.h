#pragma once

#include <mpfr.h>

#include <string>

// Helpers for numbers beyond a double: MPFR values, and decimal text whose
// exponent is too large for a double to hold.
namespace BigMath {

// Bits for a scale (half the screen height, in complex-plane units). The scale
// needs digits, not range, from its mantissa: MPFR's exponent covers the range.
inline constexpr mpfr_prec_t kScalePrecision = 64;

// Bits of precision the centre coordinate and the reference orbit need at a
// given scale: enough to resolve a pixel, plus headroom for the rounding that
// accumulates over the iterations.
mpfr_prec_t precisionForScale(mpfr_srcptr scale);

// Whether `text` is a decimal number MPFR can read, such as "-0.745" or "1e-500".
bool isDecimalNumber(const std::string& text);

// Whether `text` is such a number and greater than zero.
bool isPositiveNumber(const std::string& text);

// The binary logarithm of a positive number written in decimal, such as
// "4.7e-01" or "1e500". Works for values far outside the range of a double.
// Throws std::runtime_error if `text` is not such a number.
double log2OfDecimal(const std::string& text);

// `text`, a decimal number such as "4.701977e-01" or "1e500", rounded to
// `digits` significant digits for display: "0.47", "1e+500". Text that is not
// such a number comes back as it is.
std::string roundDecimal(const std::string& text, int digits);

} // namespace BigMath
