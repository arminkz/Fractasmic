#include "BigMath.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace BigMath {

mpfr_prec_t precisionForScale(mpfr_srcptr scale)
{
    // scale is below 2^exponent, so -exponent bits place a point on the screen;
    // never go below what a double already offers.
    const long positionBits = std::max(64L, -static_cast<long>(mpfr_get_exp(scale)));
    return static_cast<mpfr_prec_t>(positionBits) + 64;
}


bool isDecimalNumber(const std::string& text)
{
    mpfr_t value;
    mpfr_init2(value, 64);
    const bool valid = !text.empty() && mpfr_set_str(value, text.c_str(), 10, MPFR_RNDN) == 0 && mpfr_number_p(value);
    mpfr_clear(value);
    return valid;
}


bool isPositiveNumber(const std::string& text)
{
    mpfr_t value;
    mpfr_init2(value, 64);
    const bool valid = !text.empty() && mpfr_set_str(value, text.c_str(), 10, MPFR_RNDN) == 0
                    && mpfr_number_p(value) && mpfr_sgn(value) > 0;
    mpfr_clear(value);
    return valid;
}


double log2OfDecimal(const std::string& text)
{
    // Mantissa and exponent are read separately, so the exponent can be
    // larger than a double's.
    const size_t exponentAt = text.find_first_of("eE");
    try {
        size_t used = 0;
        const std::string mantissaText = text.substr(0, exponentAt);
        const double mantissa = std::stod(mantissaText, &used);
        bool valid = used == mantissaText.size() && mantissa > 0.0 && std::isfinite(mantissa);

        long exponent = 0;
        if (exponentAt != std::string::npos) {
            const std::string exponentText = text.substr(exponentAt + 1);
            exponent = std::stol(exponentText, &used);
            valid = valid && used == exponentText.size();
        }

        if (valid) {
            return std::log2(mantissa) + static_cast<double>(exponent) * std::log2(10.0);
        }
    } catch (const std::exception&) {
        // Reported below.
    }
    throw std::runtime_error("\"" + text + "\" is not a positive decimal number.");
}


std::string roundDecimal(const std::string& text, int digits)
{
    mpfr_t value;
    mpfr_init2(value, kScalePrecision);
    std::string rounded = text;
    if (!text.empty() && mpfr_set_str(value, text.c_str(), 10, MPFR_RNDN) == 0) {
        char buffer[64];
        mpfr_snprintf(buffer, sizeof(buffer), "%.*Rg", digits, value);
        rounded = buffer;
    }
    mpfr_clear(value);
    return rounded;
}

} // namespace BigMath
