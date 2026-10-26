// Shared by the shaders that iterate the Mandelbrot set by perturbation:
// the reference-orbit layout, the constants, and the extended-exponent
// arithmetic. Included with #include; it declares no bindings of its own.

// Matches OrbitPoint in ReferenceOrbit.h.
struct OrbitPoint {
    vec2 mantissa;  // the value is mantissa * 2^exponent
    int exponent;
    int padding;
};

const float ESCAPE_RADIUS = 256.0;

// Exponent that stands for zero. Must match kZeroExponent in ReferenceOrbit.h.
const int ZERO_EXPONENT = -(1 << 28);

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Largest component, used to compare magnitudes. Squared lengths of values
// this small would underflow to zero in float.
float magnitude(vec2 v) {
    return max(abs(v.x), abs(v.y));
}

// ---------------------------------------------------------------------------
// Extended-exponent arithmetic
// ---------------------------------------------------------------------------

// A complex number m * 2^e. Kept normalised: the larger component of m is in
// [0.5, 1), or m is zero and e is ZERO_EXPONENT. The mantissa has float's 24
// bits of precision; the exponent has the range of an int.
struct ExtComplex {
    vec2 m;
    int e;
};

// 2^k as a float, built straight from its bit pattern. Valid for the small k
// the functions below use (well inside float's exponent range of ±126); a
// multiplication by it is exact and much cheaper than ldexp().
float powerOfTwo(int k) {
    return intBitsToFloat((127 + k) << 23);
}

ExtComplex extNormalize(vec2 m, int e) {
    float largest = magnitude(m);
    if (largest == 0.0) return ExtComplex(vec2(0.0), ZERO_EXPONENT);

    // largest = f * 2^shift with f in [0.5, 1), read from the float's exponent
    // bits; move that shift out of the mantissa and into e.
    int shift = ((floatBitsToInt(largest) >> 23) & 0xFF) - 126;
    return ExtComplex(m * powerOfTwo(-shift), e + shift);
}

ExtComplex extMul(ExtComplex a, ExtComplex b) {
    return extNormalize(complexMul(a.m, b.m), a.e + b.e);
}

// A term this many bits below the other is under the last bit of a float's
// 24-bit mantissa even after rounding, so adding it changes nothing.
const int NEGLIGIBLE_BITS = 30;

ExtComplex extAdd(ExtComplex a, ExtComplex b) {
    // The common case in the loop: one operand dwarfs the other, and the sum
    // is just the larger one, already normalised.
    if (a.e - b.e > NEGLIGIBLE_BITS) return a;
    if (b.e - a.e > NEGLIGIBLE_BITS) return b;

    // Otherwise bring the smaller operand to the larger one's exponent.
    if (a.e < b.e) {
        ExtComplex t = a;
        a = b;
        b = t;
    }
    return extNormalize(a.m + b.m * powerOfTwo(b.e - a.e), a.e);
}

// |a| < |b|, comparing by largest component as magnitude() does.
bool extLess(ExtComplex a, ExtComplex b) {
    return a.e < b.e || (a.e == b.e && magnitude(a.m) < magnitude(b.m));
}
