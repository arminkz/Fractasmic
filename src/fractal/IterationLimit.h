#pragma once

// Choosing the iteration limit automatically, from what the pixels did.
// Pixels that escape just under the limit are the sign that more would escape
// above it, so the limit doubles while more than kTailFraction of the pixels
// escape in its top half, [limit / 2, limit).
//
// Near the boundary that share often halves with every doubling rather than
// dropping to zero, so a stricter fraction costs a lot of iterations for
// little visible gain: at this one, a fraction of a percent of the pixels come
// out black that would escape later.
//
// The rule cannot tell a pixel inside the set from one that needs far more
// iterations than the limit: neither escapes. A limit far below what an image
// needs would therefore look fine to it. So it is always applied starting
// close to a limit that suited a neighbouring image: the previous frame, or
// the previous keyframe. The escape counts rise gradually with depth, and the
// doubling follows them.

// Share of the pixels escaping in the top half of the limit that calls for
// doubling it.
inline constexpr double kTailFraction = 0.005;

// The lowest limit chosen automatically.
inline constexpr int kMinAutoIterations = 256;
