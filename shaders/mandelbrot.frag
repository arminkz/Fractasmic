#version 450

// Matches FloatFractalRenderer::PushConstants.
layout(push_constant) uniform PushConstants {
    vec2 center;        // complex-plane point at the middle of the screen
    vec2 resolution;    // framebuffer size in pixels
    float scale;        // half the screen height, in complex-plane units
    float palettePhase;
    int maxIters;
} pc;

layout(location = 0) out vec4 outColor;

const float ESCAPE_RADIUS = 256.0;

vec3 palette(float t) {
    return 0.5 + 0.5 * cos(pc.palettePhase + t * 0.15 + vec3(1.0, 2.0, 3.0));
}

// Smooth iteration count, or 0 for points inside the set.
float mandelbrot(vec2 c) {
    
    float c2 = dot(c, c);

    // Main cardioid and period-2 bulb never escape; skip the loop for them.
    if (256.0 * c2 * c2 - 96.0 * c2 + 32.0 * c.x - 3.0 < 0.0) return 0.0;
    if (16.0 * (c2 + 2.0 * c.x + 1.0) - 1.0 < 0.0) return 0.0;

    vec2 z = vec2(0.0);
    int n = 0;
    for (; n < pc.maxIters; n++) {
        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c; // zₙ₊₁ = zₙ² + c

        // if orbit of c (zₙ) explodes to infinity. Then c is outside of the mandelbrot set.
        if (dot(z, z) > ESCAPE_RADIUS * ESCAPE_RADIUS) break; 
    }

    if (n >= pc.maxIters) return 0.0;

    return float(n) - log2(log2(dot(z, z))) + 4.0;
}

void main() {

    vec2 uv = (2.0 * gl_FragCoord.xy - pc.resolution) / pc.resolution.y;

    // Vulkan's y points down; flip it so the imaginary axis points up.
    uv.y = -uv.y; 

    vec2 c = pc.center + uv * pc.scale;
    float l = mandelbrot(c);

    // select color based on mandelbrot's output
    vec3 col = (l < 0.5) ? vec3(0.0) : palette(l);

    outColor = vec4(col, 1.0);
}
