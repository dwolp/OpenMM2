// Composites the 3D scene onto the window. With OpenMM2's image filters on
// (render::PostProcess, the Tweaks page; not in MM2) it also anti-aliases,
// sharpens, grades and vignettes; with them off it is a plain copy.

TEXTURE0 uScene;
TEXTURE1 uTexture1; // the bloom chain's level 0 (bloom.frag); unread without bloom

DRAW_BLOCK {
    vec4 grade;  // contrast, saturation, warmth, vignette
    vec4 detail; // sharpen, fxaa (0/1), active (0/1), bloom intensity
    vec4 extra;  // exposure, unused
} post;

IN(0) vec2 vUv;

layout(location = 0) out vec4 oColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Bilinear sampling by hand, so the filters see the same neighbours whatever
// sampler the backend binds (point at 100% render scale).
vec3 texel(ivec2 p) {
    ivec2 size = textureSize(uScene, 0);
    return texelFetch(uScene, clamp(p, ivec2(0), size - 1), 0).rgb;
}
vec3 scene(vec2 uv) {
    vec2 t = uv * vec2(textureSize(uScene, 0)) - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = fract(t);
    vec3 a = mix(texel(i), texel(i + ivec2(1, 0)), f.x);
    vec3 b = mix(texel(i + ivec2(0, 1)), texel(i + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
}

// Fast approximate anti-aliasing in its simple directional form: blur along
// the edge the four diagonal neighbours' luma gradient finds, keeping the
// wider blur only when it stays within the neighbourhood's luma range.
vec3 fxaa(vec2 uv, vec2 px, vec3 m) {
    float lM = luma(m);
    float lNW = luma(scene(uv + vec2(-px.x, -px.y)));
    float lNE = luma(scene(uv + vec2(px.x, -px.y)));
    float lSW = luma(scene(uv + vec2(-px.x, px.y)));
    float lSE = luma(scene(uv + vec2(px.x, px.y)));
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2(-8.0), vec2(8.0)) * px;
    vec3 a = 0.5 * (scene(uv + dir * (1.0 / 3.0 - 0.5)) + scene(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (scene(uv - dir * 0.5) + scene(uv + dir * 0.5));
    float lB = luma(b);
    return (lB < lMin || lB > lMax) ? a : b;
}

void main() {
    if (post.detail.z < 0.5) { // filters off: the plain copy
        oColor = vec4(texture(uScene, vUv).rgb, 1.0);
        return;
    }
    vec2 px = 1.0 / vec2(textureSize(uScene, 0));
    vec3 c = scene(vUv);
    vec3 n = scene(vUv + vec2(0.0, -px.y));
    vec3 s = scene(vUv + vec2(0.0, px.y));
    vec3 e = scene(vUv + vec2(px.x, 0.0));
    vec3 w = scene(vUv + vec2(-px.x, 0.0));

    if (post.detail.y > 0.5)
        c = fxaa(vUv, px, c);

    // Contrast-adaptive sharpening (after AMD FidelityFX CAS's idea): the
    // negative cross-shaped lobe is weaker where the neighbourhood is already
    // near black or white, so edges do not ring.
    float amount = post.detail.x;
    if (amount > 0.0) {
        vec3 mn = min(c, min(min(n, s), min(e, w)));
        vec3 mx = max(c, max(max(n, s), max(e, w)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 lobe = -amp * mix(1.0 / 8.0, 1.0 / 5.0, amount);
        c = clamp((c + lobe * (n + s + e + w)) / (1.0 + 4.0 * lobe), 0.0, 1.0);
    }

    // Bloom: the chain's glow, added (screen blend, so it never clips hard).
    if (post.detail.w > 0.0) {
        vec3 glow = texture(uTexture1, vUv).rgb * (post.detail.w * 3.0);
        c = 1.0 - (1.0 - c) * (1.0 - clamp(glow, 0.0, 1.0));
    }

    // Brightness, then colour grading: contrast about mid grey, saturation about the luma, a
    // warm (orange) or cool (blue) shift.
    c *= post.extra.x;
    c = (c - 0.5) * post.grade.x + 0.5;
    c = mix(vec3(luma(c)), c, post.grade.y);
    c += post.grade.z * vec3(0.06, 0.015, -0.06);
    c = clamp(c, 0.0, 1.0);

    // Vignette: darker towards the corners.
    float r = length(vUv - 0.5) * 1.41421356;
    c *= 1.0 - post.grade.w * smoothstep(0.45, 1.0, r);

    oColor = vec4(c, 1.0);
}
