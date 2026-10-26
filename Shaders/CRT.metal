#include <metal_stdlib>
using namespace metal;

// Inverse barrel projection: the entire console UI, including scanlines,
// bends onto the face of a convex CRT. Out-of-bounds samples form the bezel.
[[ stitchable ]] float2 crtCurve(float2 position, float2 size, float curvature) {
    float2 uv = position / max(size, float2(1.0)) * 2.0 - 1.0;
    float radiusSquared = dot(uv, uv);
    uv *= 1.0 + curvature * radiusSquared;
    return (uv * 0.5 + 0.5) * size;
}
