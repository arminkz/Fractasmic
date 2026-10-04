#version 450

// Mandelbrot by perturbation: the reference orbit Zₙ of the screen centre is
// computed on the CPU in high precision, and each pixel only iterates its small
// difference δzₙ from that orbit.
//
// Down to moderate depth the differences fit in a plain float. Deeper than
// that they fall below the smallest float (about 1e-38), so they are carried as
// a float mantissa with a separate integer exponent, which has no such floor.

// Matches DeepZoomFractalRenderer::PushConstants.
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

// Matches OrbitPoint in ReferenceOrbit.h.
struct OrbitPoint {
    vec2 z;         // plain float value; zero if too small for a float
    vec2 mantissa;  // the same value as mantissa * 2^exponent
    int exponent;
    int padding;
};

// Reference orbit of the screen centre C: Z₀ = 0, Z₁ = C, Z₂ = C² + C, ...
layout(std430, set = 0, binding = 0) readonly buffer ReferenceOrbit {
    OrbitPoint points[];
} orbit;

layout(location = 0) out vec4 outColor;

const float ESCAPE_RADIUS = 256.0;

// Scales at or above 2^this use the plain float path. Below it, δz² and the
// offsets between neighbouring samples get too close to float's floor.
const int FLOAT_PATH_MIN_EXPONENT = -80;

// Exponent that stands for zero. Must match kZeroExponent in ReferenceOrbit.h.
const int ZERO_EXPONENT = -(1 << 28);

vec3 palette(float t) {
    return 0.5 + 0.5 * cos(pc.palettePhase + t * 0.15 + vec3(1.0, 2.0, 3.0));
}

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Largest component, used to compare magnitudes. Squared lengths of values
// this small would underflow to zero in float.
float magnitude(vec2 v) {
    return max(abs(v.x), abs(v.y));
}

float smoothIterations(int n, vec2 z) {
    if (n >= pc.maxIters) return 0.0;

    return float(n) - log2(log2(dot(z, z))) + 4.0;
}

// ---------------------------------------------------------------------------
// Float path
// ---------------------------------------------------------------------------

// Smooth iteration count, or 0 for points inside the set.
// dc is the pixel's offset from the reference point C.
float mandelbrot(vec2 dc) {
    vec2 dz = vec2(0.0);    // δzₙ, the pixel's offset from the reference orbit
    vec2 z = vec2(0.0);     // zₙ, the pixel's actual orbit value
    int ref = 0;            // index into the reference orbit
    int n = 0;
    for (; n < pc.maxIters; n++) {
        // δzₙ₊₁ = 2·Zₙ·δzₙ + δzₙ² + δc
        dz = complexMul(2.0 * orbit.points[ref].z + dz, dz) + dc;
        ref++;

        // zₙ = Zₙ + δzₙ
        z = orbit.points[ref].z + dz;

        // if orbit of c (zₙ) explodes to infinity. Then c is outside of the mandelbrot set.
        if (dot(z, z) > ESCAPE_RADIUS * ESCAPE_RADIUS) break;

        // Rebase: when z is smaller than δz, the sum above has cancelled away
        // its significant digits. Carry z on as the new δz against the start of
        // the reference orbit (Z₀ = 0), where it is exact again. The same move
        // handles running off the end of the reference orbit.
        if (magnitude(z) < magnitude(dz) || ref == pc.orbitLength - 1) {
            dz = z;
            ref = 0;
        }
    }

    return smoothIterations(n, z);
}

// ---------------------------------------------------------------------------
// Extended-exponent path
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

ExtComplex orbitPoint(int index) {
    return ExtComplex(orbit.points[index].mantissa, orbit.points[index].exponent);
}

// The same loop as mandelbrot(), with δz, δc and Z in extended-exponent form.
float mandelbrotExtended(ExtComplex dc) {
    ExtComplex dz = ExtComplex(vec2(0.0), ZERO_EXPONENT);
    vec2 z = vec2(0.0);     // only needed as a plain float once it gets large
    int ref = 0;
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

        // Rebase, as in mandelbrot().
        if (extLess(zExt, dz) || ref == pc.orbitLength - 1) {
            dz = zExt;
            ref = 0;
        }
    }

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

    // δc = offset * scale, in whichever form the depth calls for.
    float l;
    if (pc.scaleExponent >= FLOAT_PATH_MIN_EXPONENT) {
        l = mandelbrot(offset * ldexp(pc.scaleMantissa, pc.scaleExponent));
    } else {
        l = mandelbrotExtended(extNormalize(offset * pc.scaleMantissa, pc.scaleExponent));
    }

    // select color based on mandelbrot's output
    vec3 col = (l < 0.5) ? vec3(0.0) : palette(l);

    outColor = vec4(col, 1.0);
}
