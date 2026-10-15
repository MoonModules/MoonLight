#pragma once

#include "light/powerfunctions/shader.h"

#include <cmath>

/// Rendering a 3D scene by walking a ray through a distance field.
///
/// A scene is described as a function saying how far the nearest surface is from any point, so geometry emerges from arithmetic rather than being stored.
///
/// @attribution John Hart, Iñigo Quilez
///
/// @technical
///
/// ## Sphere tracing
///
/// The field guarantees nothing is nearer than the value it returns, so a ray jumps exactly that far without passing through anything.
/// That guarantee is why it converges in tens of steps rather than thousands.
/// It is also why a scene costs the same whether it holds one sphere or a thousand.
///
/// ## Float, deliberately and locally
///
/// A raymarch is per-pixel float by nature, each ray taking many square roots, so this header compiles only where the platform declares a hardware FPU.
/// Everything in the shader vocabulary stays fixed point and runs everywhere: this is the one bounded exception, gated as a whole header rather than weakening a rule in place.
///
/// The primitives, the gradient normal and the operators follow Iñigo Quilez's descriptions, implemented fresh.

#if MM_HEAVY_COMPUTE

namespace mm::raymarch {

/// A point in 3D, in plain floats, which keeps a scene function readable.
struct Vec3 {
    float x = 0, y = 0, z = 0;   ///< the three axes
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalise(Vec3 v) {
    const float l = length(v);
    return l > 1e-6f ? v * (1.0f / l) : Vec3{0, 1, 0};
}

// --- 3D distance primitives, on the same contract the 2D family follows ---------------------------

inline float sdSphere(Vec3 p, Vec3 centre, float r) { return length(p - centre) - r; }

inline float sdPlane(Vec3 p, float height) { return p.y - height; }

inline float sdBox(Vec3 p, Vec3 centre, Vec3 half) {
    const Vec3 d{std::fabs(p.x - centre.x) - half.x,
                 std::fabs(p.y - centre.y) - half.y,
                 std::fabs(p.z - centre.z) - half.z};
    const Vec3 outside{d.x > 0 ? d.x : 0, d.y > 0 ? d.y : 0, d.z > 0 ? d.z : 0};
    const float inside = std::fmin(std::fmax(d.x, std::fmax(d.y, d.z)), 0.0f);
    return length(outside) + inside;
}

/// A donut lying in the XZ plane: `r` is the ring radius, `t` the tube thickness.
inline float sdTorus(Vec3 p, Vec3 centre, float r, float t) {
    const Vec3 q = p - centre;
    const float xz = std::sqrt(q.x * q.x + q.z * q.z) - r;
    return std::sqrt(xz * xz + q.y * q.y) - t;
}

// --- Operators: the 3D counterparts, so a scene is composed rather than drawn ---------------------

inline float opUnion(float a, float b) { return a < b ? a : b; }
inline float opIntersect(float a, float b) { return a > b ? a : b; }
/// Shape `b` with shape `a` cut out of it: Quilez's operand order, matching shader.h's 2D form.
inline float opSubtract(float a, float b) { return -a > b ? -a : b; }

/// Smooth union: the operator that makes two surfaces flow into one another instead of merely
inline float smin(float a, float b, float k) {
    if (k <= 0.0f) return opUnion(a, b);
    float h = 0.5f + 0.5f * (b - a) / k;
    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
    return b + (a - b) * h - k * h * (1.0f - h);
}

/// Tile space so one object becomes an endless lattice of itself: the 3D `repeat`.
inline Vec3 opRepeat(Vec3 p, Vec3 spacing) {
    const auto fold = [](float v, float s) {
        if (s <= 0.0f) return v;
        return v - s * std::floor(v / s + 0.5f);
    };
    return {fold(p.x, spacing.x), fold(p.y, spacing.y), fold(p.z, spacing.z)};
}

// --- The renderer ----------------------------------------------------------------------------------

/// What a ray found.
struct Hit {
    bool  hit = false;   ///< whether the ray reached a surface before leaving
    float dist = 0;      ///< how far along the ray
    Vec3  point;         ///< where it landed
    Vec3  normal;        ///< the surface orientation there
};

/// The surface normal as the GRADIENT of the distance field: four extra samples, no geometry.
template <typename SceneFn>
inline Vec3 normalAt(SceneFn scene, Vec3 p) {
    constexpr float e = 0.005f;
    const float d1 = scene(Vec3{p.x + e, p.y - e, p.z - e});
    const float d2 = scene(Vec3{p.x - e, p.y - e, p.z + e});
    const float d3 = scene(Vec3{p.x - e, p.y + e, p.z - e});
    const float d4 = scene(Vec3{p.x + e, p.y + e, p.z + e});
    return normalise({d1 - d2 - d3 + d4, -d1 - d2 + d3 + d4, -d1 + d2 - d3 + d4});
}

/// March a ray through `scene` until it hits a surface or leaves. `scene` is any callable taking a
template <typename SceneFn>
inline Hit march(SceneFn scene, Vec3 origin, Vec3 dir, uint8_t maxSteps = 48,
                 float maxDist = 20.0f, float epsilon = 0.002f) {
    float t = 0.0f;
    for (uint8_t i = 0; i < maxSteps; i++) {
        const Vec3 p = origin + dir * t;
        const float d = scene(p);
        if (d < epsilon) return {true, t, p, normalAt(scene, p)};
        t += d;
        if (t > maxDist) break;
    }
    return {};
}

/// Diffuse lighting from one direction, with an ambient floor so the dark side is not pure black.
inline uint8_t diffuse(Vec3 normal, Vec3 lightDir, uint8_t ambient = 38) {
    const float d = dot(normal, normalise(lightDir));
    const float lit = (d > 0 ? d : 0) * (1.0f - ambient / 255.0f) + ambient / 255.0f;
    const int v = static_cast<int>(lit * 255.0f);
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/// A camera: where it stands, what it looks at, and how wide it sees.
///
/// @technical Building a pixel's ray is what every effect would otherwise re-derive, and getting it wrong puts a scene half off-screen.
struct Camera {
    Vec3  position{0, 1, -3};
    Vec3  target{0, 0, 0};
    float focal = 1.2f;        ///< smaller = wider field of view

    /// The ray through shader-space coordinates `sx`, `sy` (16.16, short side spanning -1..1).
    Vec3 ray(int32_t sx, int32_t sy) const {
        const Vec3 fwd = normalise(target - position);
        const Vec3 worldUp{0, 1, 0};
        // right = normalize(cross(fwd, worldUp)): the standard look-at basis.
        Vec3 right = normalise({fwd.y * worldUp.z - fwd.z * worldUp.y,
                                fwd.z * worldUp.x - fwd.x * worldUp.z,
                                fwd.x * worldUp.y - fwd.y * worldUp.x});
        const Vec3 up{right.y * fwd.z - right.z * fwd.y,
                      right.z * fwd.x - right.x * fwd.z,
                      right.x * fwd.y - right.y * fwd.x};
        const float u = static_cast<float>(sx) / 65536.0f;
        // Panel rows grow downward and the world's up points the other way, so the sign flips here once.
        const float v = -static_cast<float>(sy) / 65536.0f;
        return normalise(right * u + up * v + fwd * focal);
    }
};

}  // namespace mm::raymarch

#endif  // MM_HEAVY_COMPUTE
