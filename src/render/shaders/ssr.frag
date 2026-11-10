// OpenMM2 wet roads (render::PostProcess::wetness; not in MM2): screen-
// space reflections on the ground. Surfaces facing up (the rebuilt normal
// within ~30 degrees of the world's up) mirror the scene: a ray along the
// mirrored view direction is marched through the distance image
// (depthcopy.frag) and refined where it passes behind the scene; its colour
// is mixed in by a Fresnel term (strong at the grazing angles a driver sees
// the road at) and a puddle pattern in world space, with rain ripples
// moving the reflection. Wet ground is also a little darker. Grass (green
// ground, as mesh.frag's grassGreen tells it) stays matt, and a ray that
// leaves the picture or finds nothing reflects the sky near the horizon
// (ssr.sky) instead of stopping at a hard edge.

TEXTURE0 uTexture0; // the scene so far
TEXTURE1 uTexture1; // distances (r)

DRAW_BLOCK {
    vec4 params; // proj[0][0], proj[1][1], wetness (0..1), time (s)
    vec4 up;     // the world's up in view space (xyz), far plane
    vec4 cam0;   // the camera's x axis in the world (xyz), its position x
    vec4 cam1;   // y axis, position y
    vec4 cam2;   // z axis, position z
    vec4 sky;    // the sky near the horizon (rgb); w 1 when known
    vec4 heat;   // OpenMM2 heat shimmer: strength (0: none), unused
} ssr;

layout(location = 0) out vec4 oColor;

vec2 texelSize() { return 1.0 / vec2(textureSize(uTexture1, 0)); }
float distanceAt(vec2 uv) { return textureLod(uTexture1, uv, 0.0).r; }

// True view space (x right, y up, looking down -z) from the image's uv,
// whichever way the backend stores rows.
float ndcY(vec2 uv) {
#ifdef MM2_VULKAN
    return 1.0 - 2.0 * uv.y; // row 0 at the top
#else
    return 2.0 * uv.y - 1.0;
#endif
}
float uvY(float y) {
#ifdef MM2_VULKAN
    return 0.5 - 0.5 * y;
#else
    return 0.5 + 0.5 * y;
#endif
}
vec3 viewPos(vec2 uv, float t) {
    return vec3((uv.x * 2.0 - 1.0) * t / ssr.params.x, ndcY(uv) * t / ssr.params.y, -t);
}
vec2 project(vec3 p) {
    vec2 ndc = vec2(p.x * ssr.params.x, p.y * ssr.params.y) / -p.z;
    return vec2(ndc.x * 0.5 + 0.5, uvY(ndc.y));
}
vec3 worldPos(vec3 v) {
    return vec3(ssr.cam0.w, ssr.cam1.w, ssr.cam2.w) + ssr.cam0.xyz * v.x + ssr.cam1.xyz * v.y + ssr.cam2.xyz * v.z;
}

// An integer hash of a lattice point (a sine hash runs out of float
// precision at the city's coordinates, thousands of metres out, and the
// puddles and ripples there turned blocky).
float hash(vec2 p) {
    uvec2 v = uvec2(ivec2(floor(p))) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float(v.y >> 8u) * (1.0 / 16777216.0);
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

// OpenMM2 heat shimmer (ssr.heat.x; not in MM2), on dry ground: hot
// asphalt seen at a grazing angle, from some 30 m out to 600 m, wavers (the
// picture there read through noise rising up the screen, in pixels of a
// 1080-line picture so it looks alike at any resolution), and where the
// view grazes it most, far off, it mirrors the sky: the "wet" look of a
// road on a hot day.
vec3 heatShimmer(vec2 uv, vec2 px, vec3 p, vec3 n, float flat_, vec3 c) {
    float t = -p.z;
    vec3 view = normalize(p);
    float graze = 1.0 - abs(dot(view, n));
    // Not over water (blue or teal: the bay stays cool).
    float cool = smoothstep(0.06, 0.14, max(c.g, c.b) - c.r);
    float s = ssr.heat.x * flat_ * (1.0 - cool) * smoothstep(15.0, 50.0, t) * (1.0 - smoothstep(350.0, 600.0, t)) *
              smoothstep(0.9, 0.985, graze);
    if (s <= 0.001)
        return c;
    float time = ssr.params.w;
    float lines = 1.0 / px.y;
    vec2 sp = uv * vec2(1080.0 * px.y / px.x, 1080.0); // 1080-line pixels
    vec2 wob = vec2(noise(vec2(sp.x * 0.09, sp.y * 0.22 - time * 6.0)),
                    noise(vec2(sp.x * 0.07 + 17.0, sp.y * 0.18 - time * 5.0))) - 0.5;
    vec2 o = wob * (2.6 * s) * (lines / 1080.0) * px;
    c = textureLod(uTexture0, uv + o, 0.0).rgb;
    if (ssr.sky.w > 0.5)
        c = mix(c, ssr.sky.rgb, s * 0.3 * smoothstep(0.985, 0.999, graze) * smoothstep(60.0, 200.0, t));
    return c;
}

void main() {
    vec2 px = texelSize();
    vec2 uv = gl_FragCoord.xy * px;
    vec4 base = textureLod(uTexture0, uv, 0.0);
    oColor = base;
    float wet = ssr.params.z;
    bool heat = wet <= 0.0 && ssr.heat.x > 0.0; // never both: a wet road is not hot
    float t = distanceAt(uv);
    if ((wet <= 0.0 && !heat) || t >= ssr.up.w * 0.999 || t > (heat ? 600.0 : 250.0))
        return;
    vec3 p = viewPos(uv, t);

    // The surface's normal from the neighbours (the closer side, as ao.frag).
    vec3 r = viewPos(uv + vec2(px.x, 0.0), distanceAt(uv + vec2(px.x, 0.0))) - p;
    vec3 l = p - viewPos(uv - vec2(px.x, 0.0), distanceAt(uv - vec2(px.x, 0.0)));
    vec3 u = viewPos(uv + vec2(0.0, px.y), distanceAt(uv + vec2(0.0, px.y))) - p;
    vec3 d = p - viewPos(uv - vec2(0.0, px.y), distanceAt(uv - vec2(0.0, px.y)));
    vec3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(u.z) < abs(d.z) ? u : d));
    vec3 up = normalize(ssr.up.xyz);
    if (dot(n, up) < 0.0)
        n = -n;
    float flat_ = smoothstep(0.82, 0.92, dot(n, up));
    // Grass and other green ground soaks the rain up: no mirror on it.
    float green = smoothstep(0.02, 0.12, (base.g - max(base.r, base.b)) / max(base.g, 0.02));
    flat_ *= 1.0 - green;
    if (flat_ <= 0.0)
        return;
    if (heat) {
        oColor = vec4(heatShimmer(uv, px, p, n, flat_, base.rgb), base.a);
        return;
    }

    // Puddles: two octaves of noise over the ground; between them a thin
    // wet sheen. Rain ripples tilt the mirror a little.
    vec3 w = worldPos(p);
    float puddle = smoothstep(0.42, 0.62, noise(w.xz * 0.18) * 0.65 + noise(w.xz * 0.61) * 0.35);
    float reflectivity = mix(0.35, 1.0, puddle) * wet * flat_;
    vec2 ripple = vec2(noise(w.xz * 3.1 + ssr.params.w * 1.7), noise(w.zx * 3.3 - ssr.params.w * 1.9)) - 0.5;
    vec3 tilt = vec3(ripple.x, 0.0, ripple.y); // in the world; to view space with the camera's axes
    tilt = vec3(dot(tilt, ssr.cam0.xyz), dot(tilt, ssr.cam1.xyz), dot(tilt, ssr.cam2.xyz));
    // About the surface's own normal, not the world's up: on a hill the
    // world's up sends the rays into the rising road ahead, which then
    // mirrored itself in a dark, ragged band.
    vec3 mirror = normalize(n + tilt * mix(0.025, 0.01, puddle));

    vec3 view = normalize(p);
    vec3 rd = reflect(view, mirror);
    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(-view, mirror), 0.0, 1.0), 5.0);

    // March: finer steps that grow with distance, up to ~150 m; a hit
    // counts only within a thin shell behind the surface, so the rays do not
    // catch on the far side of thin objects (the stair-steps).
    vec3 q = p + n * 0.05;
    float stepLen = 0.12 + t * 0.005;
    vec2 hitUv = vec2(-1.0);
    float travelled = 0.0;
    vec2 lastUv = uv;
    for (int i = 0; i < 96; ++i) {
        vec3 next = q + rd * stepLen;
        vec2 s = project(next);
        if (next.z > -0.05 || s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0)
            break;
        lastUv = s;
        float sceneT = distanceAt(s);
        float behind = -next.z - sceneT;
        if (behind > 0.0 && behind < max(0.35, stepLen * 1.5)) {
            // Refine between q and next.
            vec3 a = q, b = next;
            for (int k = 0; k < 6; ++k) {
                vec3 m = (a + b) * 0.5;
                if (-m.z - distanceAt(project(m)) > 0.0)
                    b = m;
                else
                    a = m;
            }
            hitUv = project(b);
            travelled += length(b - q);
            break;
        }
        travelled += stepLen;
        q = next;
        stepLen *= 1.06;
    }
    vec3 c = base.rgb * mix(1.0, 0.82, wet * flat_); // wet ground is darker
    // What the ray finds: the scene where it hits, faded towards the sky
    // near the picture's edges and far away; the sky where it finds
    // nothing (without a sky colour, nothing: the plain wet ground).
    bool hasSky = ssr.sky.w > 0.5;
    vec3 sky = ssr.sky.rgb;
    vec3 refl = sky;
    float amount = hasSky ? 1.0 : 0.0;
    if (hitUv.x >= 0.0) {
        vec2 edge = min(hitUv, 1.0 - hitUv);
        float fade = smoothstep(0.0, 0.08, min(edge.x, edge.y)) * (1.0 - smoothstep(60.0, 150.0, travelled));
        // The thin sheen between puddles blurs what it mirrors.
        vec2 o = px * mix(2.5, 0.75, puddle);
        vec3 hit = 0.25 * (textureLod(uTexture0, hitUv + vec2(o.x, o.y), 0.0).rgb +
                           textureLod(uTexture0, hitUv - vec2(o.x, o.y), 0.0).rgb +
                           textureLod(uTexture0, hitUv + vec2(o.x, -o.y), 0.0).rgb +
                           textureLod(uTexture0, hitUv + vec2(-o.x, o.y), 0.0).rgb);
        refl = hasSky ? mix(sky, hit, fade) : hit;
        amount = hasSky ? 1.0 : fade;
    }
    c = mix(c, refl, clamp(reflectivity * fresnel * amount * 1.6, 0.0, 0.85));
    oColor = vec4(c, base.a);
}
