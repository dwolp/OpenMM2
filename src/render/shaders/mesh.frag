FRAME_BLOCK {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 ambient;
    vec4 lightDir[3];
    vec4 lightColor[3];
    mat4 shadowViewProj; // OpenMM2 sun shadows
    vec4 shadowParams;   // strength (0 = none), texel (world), size (texels), softness
    vec4 surface;        // OpenMM2 surfaces: detail, relief, smooth filtering (0/1), distance (m)
    vec4 ground;         // OpenMM2: surface variation, grass height (m, 0 = no grass), grass distance (m), time (s)
} frame;

DRAW_BLOCK {
    mat4 world;
    vec4 color;
    float alphaRef;
    uint flags;
    float specular;      // kPixelLighting: highlight strength
    float specularPower; // kPixelLighting: highlight exponent
    vec4 emissive; // lit draws: the material's emissive colour
} draw;

const uint kTexture0 = 1u;
const uint kTexture1 = 2u;
const uint kAlphaTest = 4u;
const uint kLighting = 16u;
const uint kEnvMap1 = 64u;
const uint kTex1Add = 128u;
const uint kTex1Mod2x = 256u;
const uint kTex1Blend = 512u;
const uint kPixelLighting = 1024u;
const uint kReceiveShadow = 4096u;
const uint kSurface = 16384u;
const uint kGrassShell = 32768u;
const uint kGrass = 65536u;
const uint kFlatNormals = 131072u;

TEXTURE0 uTexture0;
TEXTURE1 uTexture1;
TEXTURE2 uShadow; // OpenMM2 sun shadows: the shadow map (depth), or white

IN(0) vec4 vColor;
IN(1) vec2 vUv0;
IN(2) vec2 vUv1;
IN(3) float vFog;
IN(4) vec3 vWorldNormal;
IN(5) vec3 vWorldPos;

layout(location = 0) out vec4 oColor;

// --- OpenMM2 surfaces (not in MM2): the city's ground, walls and objects --------

// Catmull-Rom bicubic sampling in 9 bilinear taps: magnified textures look
// sharp instead of blurred or blocky.
vec4 sampleBicubic(vec2 uv, vec2 size) {
    vec2 sp = uv * size;
    vec2 t1 = floor(sp - 0.5) + 0.5;
    vec2 f = sp - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 p0 = (t1 - 1.0) / size, p3 = (t1 + 2.0) / size, p12 = (t1 + w2 / w12) / size;
    vec4 r = vec4(0.0);
    r += textureLod(uTexture0, vec2(p0.x, p0.y), 0.0) * (w0.x * w0.y);
    r += textureLod(uTexture0, vec2(p12.x, p0.y), 0.0) * (w12.x * w0.y);
    r += textureLod(uTexture0, vec2(p3.x, p0.y), 0.0) * (w3.x * w0.y);
    r += textureLod(uTexture0, vec2(p0.x, p12.y), 0.0) * (w0.x * w12.y);
    r += textureLod(uTexture0, vec2(p12.x, p12.y), 0.0) * (w12.x * w12.y);
    r += textureLod(uTexture0, vec2(p3.x, p12.y), 0.0) * (w3.x * w12.y);
    r += textureLod(uTexture0, vec2(p0.x, p3.y), 0.0) * (w0.x * w3.y);
    r += textureLod(uTexture0, vec2(p12.x, p3.y), 0.0) * (w12.x * w3.y);
    r += textureLod(uTexture0, vec2(p3.x, p3.y), 0.0) * (w3.x * w3.y);
    return clamp(r, 0.0, 1.0);
}

// The base texture: bicubic where it is magnified (with smooth filtering
// on), the usual filtering where it is minified.
vec4 baseTexture() {
    vec4 plain = texture(uTexture0, vUv0);
    if ((draw.flags & kSurface) == 0u || frame.surface.z < 0.5)
        return plain;
    vec2 size = vec2(textureSize(uTexture0, 0));
    vec2 footprint = fwidth(vUv0 * size);
    float magnified = smoothstep(1.0, 0.6, max(footprint.x, footprint.y));
    return magnified > 0.0 ? mix(plain, sampleBicubic(vUv0, size), magnified) : plain;
}

// A hash of a lattice point, 0..1 (pcg3d: integer, so it stays random far
// from the origin, where a sine hash runs out of float precision).
float surfaceHash(vec3 p) {
    uvec3 v = uvec3(ivec3(floor(p))) * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float(v.x >> 8u) * (1.0 / 16777216.0);
}
float surfaceNoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(surfaceHash(i), surfaceHash(i + vec3(1, 0, 0)), u.x), mix(surfaceHash(i + vec3(0, 1, 0)), surfaceHash(i + vec3(1, 1, 0)), u.x),
                  u.y);
    float b = mix(mix(surfaceHash(i + vec3(0, 0, 1)), surfaceHash(i + vec3(1, 0, 1)), u.x),
                  mix(surfaceHash(i + vec3(0, 1, 1)), surfaceHash(i + vec3(1, 1, 1)), u.x), u.y);
    return mix(a, b, u.z);
}

// Detail grain and relief on a surface near the camera: a fine world-space
// grain (grit, speckle) and small bumps from the texture's own brightness,
// lit by the sun. The geometric normal comes from the screen derivatives,
// which also works for the pre-lit street geometry.
vec3 surfaceDetail(vec3 c) {
    float detail = frame.surface.x, relief = frame.surface.y;
    float dist = length(vWorldPos - frame.cameraPos.xyz);
    float fade = 1.0 - smoothstep(frame.surface.w * 0.5, frame.surface.w, dist);
    if (fade <= 0.0 || (detail <= 0.0 && relief <= 0.0))
        return c;
    vec3 dpx = dFdx(vWorldPos), dpy = dFdy(vWorldPos);
    vec3 cr = cross(dpx, dpy);
    float crLen = length(cr);
    if (!(crLen > 1e-12)) // a sliver seen edge-on: no usable normal (and no NaN)
        return c;
    vec3 n = cr / crLen;
    vec3 toEye = normalize(frame.cameraPos.xyz - vWorldPos);
    float facingEye = dot(n, toEye);
    if (facingEye < 0.0) {
        n = -n;
        facingEye = -facingEye;
    }
    // Surfaces seen almost edge-on get neither: their derivatives are noise.
    fade *= smoothstep(0.03, 0.12, facingEye);
    if (fade <= 0.0)
        return c;

    // Grain: three octaves at 25, 11 and 5 cm.
    float grain = surfaceNoise(vWorldPos * 4.0) * 0.5 + surfaceNoise(vWorldPos * 9.0) * 0.3 + surfaceNoise(vWorldPos * 21.0) * 0.2;
    c *= 1.0 + detail * 0.55 * (grain - 0.5) * fade;

    // Relief: the texture's brightness as height, its slope in the texture
    // turned into the world with the cotangent frame of the derivatives.
    if (relief > 0.0 && (draw.flags & kTexture0) != 0u) {
        vec2 size = vec2(textureSize(uTexture0, 0));
        vec2 t = 1.0 / size;
        const vec3 lw = vec3(0.299, 0.587, 0.114);
        float hx = dot(texture(uTexture0, vUv0 + vec2(t.x, 0.0)).rgb - texture(uTexture0, vUv0 - vec2(t.x, 0.0)).rgb, lw);
        float hy = dot(texture(uTexture0, vUv0 + vec2(0.0, t.y)).rgb - texture(uTexture0, vUv0 - vec2(0.0, t.y)).rgb, lw);
        // Local contrast: the texture's detail against its average colour
        // (its smallest mip level), deepening grooves and joints.
        float lt = dot(texture(uTexture0, vUv0).rgb, lw);
        float la = dot(textureLod(uTexture0, vUv0, 16.0).rgb, lw);
        c *= 1.0 + clamp((lt - la) / max(la, 0.05), -1.0, 1.0) * relief * 0.3 * fade;
        vec2 duvx = dFdx(vUv0), duvy = dFdy(vUv0);
        vec3 px = cross(n, dpx), py = cross(dpy, n);
        vec3 T = py * duvx.x + px * duvy.x;
        vec3 B = py * duvx.y + px * duvy.y;
        float invMax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
        vec3 b = n - (T * hx + B * hy) * invMax * (relief * 2.5);
        float bl = length(b);
        if (bl > 1e-6) {
            vec3 bumped = b / bl;
            vec3 sun = -frame.lightDir[0].xyz;
            float light = dot(frame.lightColor[0].rgb, vec3(0.333));
            c *= 1.0 + clamp(dot(bumped, sun) - dot(n, sun), -0.6, 0.6) * 1.8 * light * fade;
        }
    }
    return clamp(c, 0.0, 1.0);
}

// Large-scale variation: patches of wear and staining a few metres across,
// which break up the repeat of the city's tiled textures. Low frequency,
// so it applies at any distance without shimmering.
vec3 surfaceVariation(vec3 c) {
    float v = frame.ground.x;
    if (v <= 0.0)
        return c;
    float n = surfaceNoise(vWorldPos * 0.09) * 0.6 + surfaceNoise(vWorldPos * 0.37 + 11.0) * 0.4;
    float stain = smoothstep(0.55, 0.85, surfaceNoise(vWorldPos * 0.21 + 37.0));
    c *= 1.0 + v * 0.4 * (n - 0.5);
    c *= mix(vec3(1.0), vec3(0.93, 0.9, 0.84), stain * v);
    return c;
}

// --- OpenMM2 grass (not in MM2): the city's grass textures ---------------------

// How green a colour is (0 for the dirt in the mixed grass textures).
float grassGreen(vec3 c) { return smoothstep(0.02, 0.12, (c.g - max(c.r, c.b)) / max(c.g, 0.02)); }

// How far the blades have grown here: full near the camera, none by the
// grass distance (mesh.vert lifts the shells by the same amount).
float grassGrow(vec3 p) {
    float d = length(p - frame.cameraPos.xyz);
    return 1.0 - smoothstep(frame.ground.z * 0.6, frame.ground.z, d);
}

// Lusher and drier patches, a few metres across, on the green parts.
vec3 grassTint(vec3 c, float green) {
    vec3 p = vWorldPos;
    float n = surfaceNoise(vec3(p.x * 0.18, 0.0, p.z * 0.18)) * 0.65 + surfaceNoise(vec3(p.x * 0.7, 3.1, p.z * 0.7)) * 0.35;
    vec3 tint = mix(vec3(0.82, 1.02, 0.8), vec3(1.16, 1.08, 0.72), smoothstep(0.3, 0.75, n));
    return c * mix(vec3(1.0), tint, green);
}

// One grid of blades: one per cell, at a random place, of random height
// (shorter in clumps), tapering to its tip. Returns the blade's height
// (0..1 of the grass height) where layer h is inside a blade, else 0.
float grassCell(vec2 xz, float scale, float seed, float h, float clump, out float tint) {
    vec2 q = xz * scale;
    vec2 cell = floor(q);
    float r1 = surfaceHash(vec3(cell, seed)), r2 = surfaceHash(vec3(cell, seed + 4.0));
    float r3 = surfaceHash(vec3(cell, seed + 8.0));
    float height = (0.35 + 0.65 * r3) * clump;
    tint = r1;
    if (h > height)
        return 0.0;
    vec2 centre = 0.2 + 0.6 * vec2(r1, r2);
    float radius = 0.34 * (1.0 - h / height) + 0.03;
    return length(fract(q) - centre) < radius ? height : 0.0;
}

// A shell layer (kGrassShell): the blade at this point, swaying in the
// wind, shaded darker towards the roots. Discards outside the blades.
vec3 grassShell(vec3 c, float green) {
    float h = draw.specular; // the layer, 0..1 of the grass height
    float grow = grassGrow(vWorldPos) * green;
    if (grow <= 0.02)
        discard;
    float t = frame.ground.w;
    vec2 xz = vWorldPos.xz;
    float sway = 0.5 + 0.5 * sin(t * 1.6 + dot(xz, vec2(0.45, 0.3))) + 0.3 * sin(t * 3.7 + xz.x * 1.3);
    xz -= vec2(0.8, 0.5) * (sway * 0.04 * h * h);
    // Taller and shorter clumps, then three grids of blades at different
    // sizes and angles, so no rows show.
    float clump = 0.55 + 0.6 * surfaceNoise(vec3(xz.x * 1.7, 0.0, xz.y * 1.7));
    float tint;
    float blade = grassCell(mat2(0.866, 0.5, -0.5, 0.866) * xz, 47.0, 1.0, h / grow, clump, tint);
    if (blade <= 0.0)
        blade = grassCell(mat2(0.643, -0.766, 0.766, 0.643) * xz + vec2(0.37, 0.71), 39.0, 17.0, h / grow, clump, tint);
    if (blade <= 0.0)
        blade = grassCell(mat2(0.985, 0.174, -0.174, 0.985) * xz + vec2(0.11, 0.53), 31.0, 33.0, h / grow, clump, tint);
    if (blade <= 0.0)
        discard;
    // Roots in the shade of the others, tips in the light; each blade a
    // slightly different green, some yellower.
    c *= 0.82 + 0.4 * h;
    c *= mix(vec3(0.88, 1.0, 0.86), vec3(1.15, 1.08, 0.75), tint * tint);
    return c;
}

// OpenMM2 sun shadows (not in MM2): how much of the sun reaches this point,
// 0..1: a 5x5 bilinear percentage-closer filter, which compares a 6x6 block
// of shadow-map texels weighted by where the point falls between them, for
// soft edges. The point is moved along its normal first (more where the
// sun grazes it), so surfaces do not shadow themselves. Shadows fade out
// in a ring well inside the map's edge, so nothing pops as the map moves.
float sunVisibility(vec3 n, float facing) {
    vec3 p = vWorldPos + n * (frame.shadowParams.y * (1.5 + 2.5 * (1.0 - facing)));
    vec4 s = frame.shadowViewProj * vec4(p, 1.0);
    vec3 ndc = s.xyz / s.w;
#ifdef MM2_VULKAN
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); // row 0 at the top
#else
    vec2 uv = ndc.xy * 0.5 + 0.5; // row 0 at the bottom
#endif
    float ring = length(uv - 0.5) * 2.0;
    if (ring >= 0.95 || ndc.z >= 1.0 || ndc.z <= 0.0)
        return 1.0;
    float size = frame.shadowParams.z;
    // Softness: the filter's grid is this many texels apart (1: every texel).
    float spacing = max(frame.shadowParams.w, 0.5);
    vec2 t = uv * size / spacing - 0.5;
    ivec2 base = ivec2(floor(t)) - 2;
    vec2 f = fract(t);
    float wx[6] = float[6](1.0 - f.x, 1.0, 1.0, 1.0, 1.0, f.x);
    float wy[6] = float[6](1.0 - f.y, 1.0, 1.0, 1.0, 1.0, f.y);
    ivec2 top = ivec2(int(size) - 1);
    float lit = 0.0;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            ivec2 at = ivec2((vec2(base + ivec2(x, y)) + 0.5) * spacing);
            float d = texelFetch(uShadow, clamp(at, ivec2(0), top), 0).r;
            lit += (ndc.z <= d + 0.0002 ? 1.0 : 0.0) * wx[x] * wy[y];
        }
    lit /= 25.0;
    return mix(lit, 1.0, smoothstep(0.7, 0.95, ring));
}

void main() {
    vec4 c = vColor;
    // OpenMM2 enhanced lighting (kPixelLighting; not in MM2). The same
    // Direct3D 7 formula as mesh.vert, per pixel and with the normal
    // renormalised, plus a Blinn-Phong highlight from the key light added
    // after the texture.
    bool pixel = (draw.flags & kPixelLighting) != 0u;
    vec3 n = vec3(0.0, 1.0, 0.0);
    vec3 toEye = vec3(0.0, 0.0, 1.0);
    if (pixel) {
        n = normalize(vWorldNormal);
        toEye = normalize(frame.cameraPos.xyz - vWorldPos);
    }
    vec3 highlight = vec3(0.0);
    if (pixel && (draw.flags & kLighting) != 0u) {
        vec3 light = frame.ambient.rgb;
        for (int i = 0; i < 3; ++i)
            light += frame.lightColor[i].rgb * max(dot(n, -frame.lightDir[i].xyz), 0.0);
        c.rgb = clamp(draw.emissive.rgb + c.rgb * light, 0.0, 1.0);
        vec3 toLight = -frame.lightDir[0].xyz;
        if (draw.specular > 0.0 && dot(n, toLight) > 0.0) {
            float nh = max(dot(n, normalize(toLight + toEye)), 0.0);
            highlight = frame.lightColor[0].rgb * (draw.specular * pow(nh, draw.specularPower));
        }
    }
    if ((draw.flags & kTexture0) != 0u)
        c *= baseTexture();
    // OpenMM2 grass: patches, the shells' blades and the soil between them.
    if ((draw.flags & kGrass) != 0u && frame.ground.y > 0.0) {
        float green = grassGreen(c.rgb);
        c.rgb = grassTint(c.rgb, green);
        if ((draw.flags & kGrassShell) != 0u)
            c.rgb = grassShell(c.rgb, green);
        else
            c.rgb *= 1.0 - 0.15 * green * grassGrow(vWorldPos);
    }
    if ((draw.flags & kSurface) != 0u)
        c.rgb = surfaceDetail(surfaceVariation(c.rgb));
    if ((draw.flags & kTexture1) != 0u) {
        vec2 uv1 = vUv1;
        if (pixel && (draw.flags & kEnvMap1) != 0u) {
            // The sphere map from the per-pixel normal (mesh.vert's mapping),
            // stronger at grazing angles as paint reflects (Schlick's
            // Fresnel shape: x0.5 facing the camera up to x1.5 edge on).
            uv1 = vec2(n.x, -n.y) * 0.5 + 0.5;
            float facing = 1.0 - max(dot(n, toEye), 0.0);
            c.rgb *= 0.5 + facing * facing * facing * facing * facing;
        }
        vec4 t1 = texture(uTexture1, uv1);
        if ((draw.flags & kTex1Add) != 0u)
            c.rgb = min(c.rgb + t1.rgb, vec3(1.0));
        else if ((draw.flags & kTex1Mod2x) != 0u)
            c.rgb = min(c.rgb * t1.rgb * 2.0, vec3(1.0));
        else if ((draw.flags & kTex1Blend) != 0u)
            c.rgb = mix(c.rgb, t1.rgb, t1.a);
        else
            c *= t1;
    }
    // D3DCMP_GREATEREQUAL against the reference value.
    if ((draw.flags & kAlphaTest) != 0u && c.a < draw.alphaRef)
        discard;
    // OpenMM2 sun shadows: darken what the sun does not reach (the game's
    // colours are mostly pre-lit, so the whole colour; the highlight too).
    // The darkening is weighted by how much the surface faces the sun:
    // a side facing away got no sunlight in the game's own lighting either,
    // and this keeps grazing surfaces from flickering in and out of shadow.
    if (frame.shadowParams.x > 0.0 && (draw.flags & kReceiveShadow) != 0u) {
        vec3 wn = vWorldNormal;
        if ((draw.flags & kFlatNormals) != 0u) {
            // The street's walls carry the same up normal as its roads: the
            // surface's own, facing the camera (its front faces do).
            vec3 g = cross(dFdx(vWorldPos), dFdy(vWorldPos));
            float gl = length(g); // tiny up close: normalise here, not below
            if (gl > 1e-12) {
                wn = g / gl;
                if (dot(wn, frame.cameraPos.xyz - vWorldPos) < 0.0)
                    wn = -wn;
            }
        }
        float len = length(wn);
        vec3 sn = len > 1e-5 ? wn / len : vec3(0.0);
        float facing = len > 1e-5 ? clamp(dot(sn, -frame.lightDir[0].xyz), 0.0, 1.0) : 1.0;
        float weight = len > 1e-5 ? smoothstep(0.0, 0.3, facing) : 1.0;
        float sun = weight > 0.0 ? sunVisibility(sn, facing) : 1.0;
        c.rgb *= 1.0 - frame.shadowParams.x * weight * (1.0 - sun);
        highlight *= sun;
    }
    c.rgb = min(c.rgb + highlight, vec3(1.0));
    c.rgb = mix(frame.fogColor.rgb, c.rgb, vFog);
    oColor = c;
}
