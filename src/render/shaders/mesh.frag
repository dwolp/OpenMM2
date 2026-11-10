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
    vec4 shadowParams;   // OpenMM2 sun shadows: strength (0 = none), cascade count, size (texels), penumbra (the sun's spread)
    mat4 shadowCascade[4];     // nearest first, in the map's 2 x 2 cells
    vec4 shadowCascadeInfo[4]; // texel (world), depth (m)
    vec4 ground;         // OpenMM2: ray traced wetness (0: none), grass height (m, 0 = no grass), grass distance (m), time (s)
    vec4 haze;           // OpenMM2 distance haze: density (1/m), height falloff (1/m), sun glow, most
    vec4 lightInfo;      // OpenMM2 night lighting: light count
    vec4 lightPos[48];   // position, range
    vec4 lightSpot[48];  // direction, cos outer (< -1: a point light)
    vec4 lightTint[48];   // colour, cos inner
} frame;

DRAW_BLOCK {
    mat4 world;
    vec4 color;
    float alphaRef;
    uint flags;
    float specular;      // kPixelLighting: highlight strength
    float specularPower; // kPixelLighting: highlight exponent
    vec4 emissive; // lit draws: the material's emissive colour; w: kPixelLighting's clear coat (0 = none)
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
const uint kEnvProbe = 2048u;
const uint kReceiveShadow = 4096u;
const uint kGrassShell = 32768u;
const uint kGrass = 65536u;
const uint kFlatNormals = 131072u;
const uint kNightLit = 262144u;
const uint kWater = 524288u;

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

// A hash of a lattice point, 0..1 (pcg3d: integer, so it stays random far
// from the origin, where a sine hash runs out of float precision).
float latticeHash(vec3 p) {
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
// Smooth value noise, 0..1.
float valueNoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(latticeHash(i), latticeHash(i + vec3(1, 0, 0)), u.x),
                  mix(latticeHash(i + vec3(0, 1, 0)), latticeHash(i + vec3(1, 1, 0)), u.x), u.y);
    float b = mix(mix(latticeHash(i + vec3(0, 0, 1)), latticeHash(i + vec3(1, 0, 1)), u.x),
                  mix(latticeHash(i + vec3(0, 1, 1)), latticeHash(i + vec3(1, 1, 1)), u.x), u.y);
    return mix(a, b, u.z);
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
    float n = valueNoise(vec3(p.x * 0.18, 0.0, p.z * 0.18)) * 0.65 + valueNoise(vec3(p.x * 0.7, 3.1, p.z * 0.7)) * 0.35;
    vec3 tint = mix(vec3(0.82, 1.02, 0.8), vec3(1.16, 1.08, 0.72), smoothstep(0.3, 0.75, n));
    return c * mix(vec3(1.0), tint, green);
}

// A blade's radius (in cells) at layer h of a blade `height` tall: tapering
// to its tip, none above it.
float grassRadius(float h, float height) { return h > height ? 0.0 : 0.34 * (1.0 - h / height) + 0.03; }

// The share of a cell that layer h of the blades covers on average: the
// mean of pi r^2 over the blades' random heights (four of them).
float grassMeanCover(float h, float clump) {
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        float r = grassRadius(h, (0.35 + 0.65 * (float(i) + 0.5) * 0.25) * clump);
        sum += r * r;
    }
    return 3.14159265 * 0.25 * sum;
}

// One grid of blades: one per cell, at a random place, of random height
// (shorter in clumps), tapering to its tip. Returns how much of the pixel
// layer h of a blade covers, with soft edges a pixel wide; where the cells
// shrink below a pixel it becomes the grid's average cover, so far grass
// does not shimmer. `pixel` is the pixel's size on the ground (m), `tint`
// the blade's own random shade.
float grassCell(vec2 xz, float scale, float seed, float h, float clump, float pixel, out float tint) {
    vec2 q = xz * scale;
    float footprint = pixel * scale; // cells per pixel
    float far = smoothstep(0.25, 0.6, footprint);
    float mean = grassMeanCover(h, clump);
    tint = 0.58; // so the average of tint^2 (1/3) holds for the far grass
    float near = 0.0;
    if (far < 1.0) {
        vec2 cell = floor(q);
        float r1 = latticeHash(vec3(cell, seed)), r2 = latticeHash(vec3(cell, seed + 4.0));
        float r3 = latticeHash(vec3(cell, seed + 8.0));
        float radius = grassRadius(h, (0.35 + 0.65 * r3) * clump);
        if (radius > 0.0) {
            vec2 centre = 0.2 + 0.6 * vec2(r1, r2);
            float d = length(fract(q) - centre);
            float soft = max(footprint, 1e-4);
            near = 1.0 - smoothstep(radius - soft, radius + soft, d);
        }
        tint = mix(r1, tint, far);
    }
    return mix(near, mean, far);
}

// A shell layer (kGrassShell): the blades' cover at this point (the alpha;
// drawn blended over the layers below), swaying in the wind, shaded darker
// towards the roots. Discards where nothing shows.
vec4 grassShell(vec3 c, float green) {
    float h = draw.specular; // the layer, 0..1 of the grass height
    // Taken before any discard, while the neighbouring pixels still run.
    vec2 fw = fwidth(vWorldPos.xz);
    float pixel = max(fw.x, fw.y);
    float grow = grassGrow(vWorldPos) * green;
    if (grow <= 0.02)
        discard;
    float t = frame.ground.w;
    vec2 xz = vWorldPos.xz;
    float sway = 0.5 + 0.5 * sin(t * 1.6 + dot(xz, vec2(0.45, 0.3))) + 0.3 * sin(t * 3.7 + xz.x * 1.3);
    xz -= vec2(0.8, 0.5) * (sway * 0.04 * h * h);
    // Taller and shorter clumps, then three grids of blades at different
    // sizes and angles, so no rows show.
    float clump = 0.55 + 0.6 * valueNoise(vec3(xz.x * 1.7, 0.0, xz.y * 1.7));
    float hn = h / grow;
    float t1, t2, t3;
    float a1 = grassCell(mat2(0.866, 0.5, -0.5, 0.866) * xz, 47.0, 1.0, hn, clump, pixel, t1);
    float a2 = grassCell(mat2(0.643, -0.766, 0.766, 0.643) * xz + vec2(0.37, 0.71), 39.0, 17.0, hn, clump, pixel, t2);
    float a3 = grassCell(mat2(0.985, 0.174, -0.174, 0.985) * xz + vec2(0.11, 0.53), 31.0, 33.0, hn, clump, pixel, t3);
    float cover = 1.0 - (1.0 - a1) * (1.0 - a2) * (1.0 - a3);
    if (cover < 0.004)
        discard;
    // Each blade's shade weighted by its cover; the first grid in front.
    float w1 = a1, w2 = a2 * (1.0 - a1), w3 = a3 * (1.0 - a1) * (1.0 - a2);
    float tint = (t1 * t1 * w1 + t2 * t2 * w2 + t3 * t3 * w3) / max(w1 + w2 + w3, 1e-4);
    // Roots in the shade of the others, tips in the light; each blade a
    // slightly different green, some yellower.
    c *= 0.95 + 0.35 * h;
    c *= mix(vec3(0.88, 1.0, 0.86), vec3(1.15, 1.08, 0.75), tint);
    return vec4(c, cover);
}

// --- OpenMM2 clear coat (not in MM2): glossy car paint -------------------------

// OpenMM2 live reflections (kEnvProbe): the world around the player's car
// along `d`, from the probe atlas in uTexture1 (six 90-degree faces, 3 x 2;
// game/ReflectionProbe.h's kProbeFaces and probeLookup, which tests pin).
vec3 probeColor(vec3 d) {
    vec3 a = abs(d);
    vec3 fwd;
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec2 cell;
    if (a.x >= a.y && a.x >= a.z) {
        fwd = vec3(d.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
        cell = d.x >= 0.0 ? vec2(0.0, 0.0) : vec2(1.0, 0.0);
    } else if (a.y >= a.z) {
        bool upward = d.y >= 0.0;
        fwd = vec3(0.0, upward ? 1.0 : -1.0, 0.0);
        up = vec3(0.0, 0.0, upward ? -1.0 : 1.0);
        cell = upward ? vec2(2.0, 0.0) : vec2(0.0, 1.0);
    } else {
        bool ahead = d.z >= 0.0;
        fwd = vec3(0.0, 0.0, ahead ? 1.0 : -1.0);
        cell = ahead ? vec2(1.0, 1.0) : vec2(2.0, 1.0);
    }
    vec3 right = cross(up, -fwd);
    float size = float(textureSize(uTexture1, 0).y) * 0.5;
    float edge = 1.0 - 3.0 / size;
    vec2 f = clamp(vec2(dot(d, right), dot(d, up)) / dot(d, fwd), -edge, edge);
    vec2 uv = vec2((cell.x + f.x * 0.5 + 0.5) / 3.0, (cell.y + 0.5 - f.y * 0.5) / 2.0);
#ifndef MM2_VULKAN
    uv.y = 1.0 - uv.y; // row 0 at the bottom
#endif
    // Four taps 0.6 texel apart (inside the 3-texel margin): glossy paint
    // rather than a flickering mirror.
    vec2 o = vec2(0.6 / (3.0 * size), 0.6 / (2.0 * size));
    return 0.25 * (textureLod(uTexture1, uv + vec2(o.x, o.y), 0.0).rgb + textureLod(uTexture1, uv - vec2(o.x, o.y), 0.0).rgb +
                   textureLod(uTexture1, uv + vec2(o.x, -o.y), 0.0).rgb + textureLod(uTexture1, uv + vec2(-o.x, o.y), 0.0).rgb);
}

// The sky as a glossy surface mirrors it along `r` (world space): the time
// of day's air colour at the horizon, bluer and darker overhead, the dim
// ground below. Chosen by eye; the game has no sky colour of its own.
vec3 coatSky(vec3 r) {
    vec3 horizon = frame.fogColor.rgb * 1.15;
    vec3 zenith = frame.fogColor.rgb * vec3(0.55, 0.72, 1.05);
    vec3 sky = mix(horizon, zenith, sqrt(max(r.y, 0.0)));
    vec3 ground = frame.fogColor.rgb * 0.16;
    // A crisp horizon line, the tell of a mirror finish along the panels.
    return mix(ground, sky, smoothstep(-0.04, 0.01, r.y));
}

// --- OpenMM2 night lighting and distance haze (not in MM2) ---------------------

// OpenMM2 glowing windows (FrameConstants::windowGlow in cameraPos.w): the
// night variants' walls are painted dark and their lit windows bright, so
// the bright texels are the windows. They shine at their own colour,
// whatever light the vertex colours bake in, bright enough for the bloom.
vec3 windowLight(vec3 t) {
    float m = max(max(t.r, t.g), t.b);
    return min(t * (smoothstep(0.36, 0.7, m) * frame.cameraPos.w), vec3(1.0));
}

// The surface's normal for the scene lights: the per-pixel one where the
// draw is lit per pixel, the vertex normal where it has a real one, else
// the surface's own from the screen derivatives (the street geometry's
// placeholders), facing the camera.
vec3 lightingNormal(bool pixel, vec3 n) {
    if (pixel)
        return n;
    if ((draw.flags & kFlatNormals) == 0u) {
        float l = length(vWorldNormal);
        if (l > 1e-5)
            return vWorldNormal / l;
    }
    vec3 g = cross(dFdx(vWorldPos), dFdy(vWorldPos));
    float gl = length(g);
    if (!(gl > 1e-12))
        return vec3(0.0, 1.0, 0.0);
    g /= gl;
    return dot(g, frame.cameraPos.xyz - vWorldPos) < 0.0 ? -g : g;
}

// The light the scene lights (street lamps, headlights, sirens) give this
// point: a softened inverse square windowed to each light's range, the
// cone for spots, and a softened facing term ((N.L)^0.3), so headlight
// beams still reach along a road they graze.
#ifdef MM2_RAY_QUERY
// Whether the scene blocks the segment from `o` along `dir` for `len`.
bool rayBlocked(vec3 o, vec3 dir, float len);
#endif
vec3 sceneLight(int i, vec3 p, vec3 n);

// `p` and `n`: the point lit and its normal. With `shadows` (ray traced:
// FrameConstants::rayLightShadows), each light that adds more than a
// little to any pixel of the 2 x 2 quad traces a ray to it, from one pixel
// of the quad (light i from pixel i % 4), which shares the answer with the
// others: the lamp posts, cars and walls between shade the point, at half
// resolution, for a quarter of the rays. (The ray stops short of the light,
// which sits inside its lamp's head or at the car's front.) The loop and
// the quad's exchanges run alike in its four pixels: no light is skipped
// before them.
vec3 surfaceNormal();
vec3 sceneLightsAt(vec3 p, vec3 n, bool shadows) {
    vec3 sum = vec3(0.0);
    int count = int(frame.lightInfo.x + 0.5);
#ifdef MM2_RAY_QUERY
    // The rays leave from off the surface along its own normal (from the
    // screen derivatives: the city models' walls carry up normals, along
    // which a ray stayed in the wall and caught it in speckles), more far
    // from the camera, where depth is coarser.
    vec3 gn = shadows ? surfaceNormal() : n;
    float lift = 0.04 + 0.0015 * length(p - frame.cameraPos.xyz);
#endif
    for (int i = 0; i < count; ++i) {
        vec3 add = sceneLight(i, p, n);
#ifdef MM2_RAY_QUERY
        if (shadows) {
            bool need = dot(add, vec3(0.333)) > 0.004;
            uint any = (need ? 1u : 0u) | subgroupQuadSwapHorizontal(need ? 1u : 0u) |
                       subgroupQuadSwapVertical(need ? 1u : 0u) | subgroupQuadSwapDiagonal(need ? 1u : 0u);
            if (any != 0u) {
                uint blocked = 0u;
                if ((gl_SubgroupInvocationID & 3u) == (uint(i) & 3u)) {
                    vec3 l = frame.lightPos[i].xyz - p;
                    float d = length(l);
                    float stop = frame.lightSpot[i].w > -1.5 ? 0.35 : 0.7;
                    vec3 off = gn * (dot(gn, l) < 0.0 ? -lift : lift);
                    if (d > stop + 0.1 && rayBlocked(p + off, l / d, d - stop))
                        blocked = 1u;
                }
                if (subgroupQuadBroadcast(blocked, uint(i) & 3u) != 0u)
                    continue;
            }
        }
#endif
        sum += add;
    }
    return sum;
}

// What scene light `i` adds at `p` (normal `n`); 0 out of its reach.
vec3 sceneLight(int i, vec3 p, vec3 n) {
    {
        vec3 l = frame.lightPos[i].xyz - p;
        float d2 = dot(l, l);
        float r = frame.lightPos[i].w;
        if (d2 >= r * r)
            return vec3(0.0);
        float d = sqrt(d2);
        l /= max(d, 1e-4);
        float spot = 1.0;
        if (frame.lightSpot[i].w > -1.5) {
            // A dipped beam's shape: as wide as the cone sideways, 40% of
            // it below the axis and 50% above, dimmer there (a soft top
            // cut-off: at 12% and then 25% it faded over a few degrees,
            // which on a road rising ahead drew a hard straight line across
            // the tarmac), so it spreads across the road ahead instead of
            // laying a round pool on it. Measured as the tangent off the
            // axis.
            vec3 axis = frame.lightSpot[i].xyz;
            float along = dot(-l, axis);
            if (along <= 0.0)
                return vec3(0.0);
            vec3 side = cross(axis, vec3(0.0, 1.0, 0.0));
            side = dot(side, side) > 1e-6 ? normalize(side) : vec3(1.0, 0.0, 0.0);
            vec3 lift = cross(side, axis);
            float x = dot(-l, side) / along, y = dot(-l, lift) / along;
            // Above the axis it also dims gradually, to 40% by 0.12 (7
            // degrees): its cut-off is a gentle fade, which a road rising
            // into the beam (a hill, wet) showed as a hard line before.
            float above = mix(1.0, 0.4, smoothstep(0.0, 0.12, y));
            y /= y > 0.0 ? 0.5 : 0.4;
            float tanOff = sqrt(x * x + y * y);
            float co = frame.lightSpot[i].w, ci = frame.lightTint[i].w;
            float tanOuter = sqrt(max(1.0 - co * co, 0.0)) / max(co, 1e-3);
            float tanInner = sqrt(max(1.0 - ci * ci, 0.0)) / max(ci, 1e-3);
            spot = 1.0 - smoothstep(tanInner, tanOuter, tanOff);
            if (spot <= 0.0)
                return vec3(0.0);
            // Squared: a soft edge fading into a brighter core, not a flat
            // disc.
            spot *= spot;
            // The ground near the lamp gets less than the throw ahead
            // (the inverse square would blow it out), over the first 30%
            // of the range.
            spot *= mix(0.35, 1.0, smoothstep(0.0, 0.3 * r, d)) * above;
        }
        // A smooth window to the range times a softened inverse square
        // (half strength at ~0.3 of the range; a beam's at ~0.55, as a
        // headlight's hot spot just under its cut-off throws far).
        float window = 1.0 - d2 / (r * r);
        float r0 = (frame.lightSpot[i].w > -1.5 ? 0.55 : 0.3) * r;
        float falloff = window * window / (1.0 + d2 / (r0 * r0));
        // Beams graze the road they light: their facing term is softer
        // still, or the road would go dark a few metres ahead.
        float facing = pow(max(dot(n, l), 0.0), frame.lightSpot[i].w > -1.5 ? 0.12 : 0.3);
        return frame.lightTint[i].rgb * (falloff * spot * facing);
    }
}
vec3 sceneLights(vec3 n) {
#ifdef MM2_RAY_QUERY
    // Not the grass shells: they discard early, and the quads' exchanges
    // need all four pixels.
    return sceneLightsAt(vWorldPos, n, frame.lightInfo.w > 0.5 && (draw.flags & kGrassShell) == 0u);
#else
    return sceneLightsAt(vWorldPos, n, false);
#endif
}

// Distance haze: light scattered by the air between the camera and the
// point, thicker low down (height falloff) and brighter towards the sun.
// Its colour is the game's fog colour (the time of day's horizon), plus
// the key light's in the sun's direction.
vec3 applyHaze(vec3 c) {
    float density = frame.haze.x;
    if (density <= 0.0)
        return c;
    vec3 d = vWorldPos - frame.cameraPos.xyz;
    float dist = length(d);
    if (dist < 1e-3)
        return c;
    float k = frame.haze.y;
    float h = 0.5 * (exp(-max(vWorldPos.y, 0.0) * k) + exp(-max(frame.cameraPos.y, 0.0) * k));
    float amount = min(1.0 - exp(-density * dist * h), frame.haze.w);
    vec3 v = d / dist;
    float toSun = max(dot(v, -frame.lightDir[0].xyz), 0.0);
    float glow = toSun * toSun * toSun * toSun;
    glow *= glow; // ^8
    vec3 air = frame.fogColor.rgb + frame.lightColor[0].rgb * (glow * frame.haze.z);
    return mix(c, min(air, vec3(1.0)), amount);
}

// OpenMM2 sun shadows (not in MM2): how much of the sun reaches this point,
// 0..1, from the cascades of the shadow map (nearest first, each in its
// cell of the map; game::sunShadowCascades). The point is moved along its
// normal first (more where the sun grazes it, by that cascade's texel), so
// surfaces do not shadow themselves. The first cascade the point is well
// inside answers, blended into the next towards its edge, and the last
// fades out towards its own, so nothing pops as the maps move.

// Where `p` falls in cascade `i`: the map texel (x, y; whole map), its depth
// (z) and how far out in the cascade it is (w, 0 centre .. 1 edge); w > 1
// when outside.
vec4 shadowCoord(int i, vec3 n, float facing) {
    vec4 info = frame.shadowCascadeInfo[i];
    vec3 p = vWorldPos + n * (info.x * (1.5 + 2.5 * (1.0 - facing)));
    vec4 s = frame.shadowCascade[i] * vec4(p, 1.0);
    vec3 ndc = s.xyz / s.w;
    if (ndc.z >= 1.0 || ndc.z <= 0.0)
        return vec4(0.0, 0.0, 0.0, 2.0);
    float cell = frame.shadowParams.z * 0.5;
    vec2 col = vec2(float(i % 2), float(i / 2));
#ifdef MM2_VULKAN
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); // row 0 at the top
    vec2 origin = col * cell;
#else
    vec2 uv = ndc.xy * 0.5 + 0.5; // row 0 at the bottom
    vec2 origin = vec2(col.x, 1.0 - col.y) * cell;
#endif
    float ring = max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0;
    return vec4(origin + uv * cell, ndc.z, ring);
}

// The lit share at map texel `at` with depth `z` in cascade `i`.
// Contact hardening (frame.shadowParams.w, the sun's spread): a 4 x 4 search
// finds the average depth of what blocks the sun around the point; the
// filter then widens with the distance from it to the point (a tower's
// shadow goes soft at its far end, a car's stays sharp at its tyres), and
// the shadow lightens with it (the sky's light reaches in under a caster
// far above; a tower's shadow fades out gradually towards its tip rather
// than ending at full darkness). Then a 5x5 bilinear percentage-closer
// filter, which compares a 6x6 block of shadow-map texels weighted by where
// the point falls between them.
float shadowLit(int i, vec2 at, float z) {
    float cell = frame.shadowParams.z * 0.5;
    vec2 col = vec2(float(i % 2), float(i / 2));
#ifdef MM2_VULKAN
    vec2 origin = col * cell;
#else
    vec2 origin = vec2(col.x, 1.0 - col.y) * cell;
#endif
    ivec2 lo = ivec2(origin), hi = ivec2(origin + cell) - 1;
    vec4 info = frame.shadowCascadeInfo[i];
    float bias = 0.12 / max(info.y, 1.0); // 12 cm along the light
    // The filter's grid is this many texels apart (1: every texel), finer
    // for the coarser cascades (their texels are larger).
    float spacing = max(frame.shadowCascadeInfo[0].x / info.x, 0.5);
    float spread = frame.shadowParams.w;
    float fade = 0.0;
    if (spread > 0.0) {
        // Search up to the penumbra of a blocker 40 m away.
        float reach = clamp(40.0 * spread / info.x, 1.0, 8.0);
        float sum = 0.0, count = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                vec2 o = (vec2(float(x), float(y)) - 1.5) * (reach / 1.5);
                float d = texelFetch(uShadow, clamp(ivec2(at + o), lo, hi), 0).r;
                if (d + bias < z) {
                    sum += d;
                    count += 1.0;
                }
            }
        if (count == 0.0)
            return 1.0; // nothing near blocks the sun
        float metres = (z - sum / count) * info.y;
        float penumbra = metres * spread / info.x; // texels
        spacing = clamp(max(spacing * 0.5, penumbra / 2.5), 0.5, 6.0);
        // Lighter by up to 65%: with the default spread, a sixth at 50 m
        // from the caster, half at 100 m, all of it from 150 m.
        fade = 0.65 * smoothstep(0.0, 1.0, metres * spread / 3.0);
    }
    vec2 t = at / spacing - 0.5;
    ivec2 base = ivec2(floor(t)) - 2;
    vec2 f = fract(t);
    float wx[6] = float[6](1.0 - f.x, 1.0, 1.0, 1.0, 1.0, f.x);
    float wy[6] = float[6](1.0 - f.y, 1.0, 1.0, 1.0, 1.0, f.y);
    float lit = 0.0;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            ivec2 p = ivec2((vec2(base + ivec2(x, y)) + 0.5) * spacing);
            float d = texelFetch(uShadow, clamp(p, lo, hi), 0).r;
            lit += (z <= d + bias ? 1.0 : 0.0) * wx[x] * wy[y];
        }
    return mix(lit / 25.0, 1.0, fade);
}

#ifdef MM2_RAY_QUERY
// OpenMM2 ray traced sun shadows (FrameConstants::shadowRayTraced): rays
// from the point, moved off its surface along the normal (more far from
// the camera, where depth is coarser), towards the sun's disc through the
// frame's ray scene (Device::beginRayScene). The disc is 0.01 rad across in
// radius (twice the real sun's, as the shadow maps' spread): two rays at
// opposite sides of it, turned per pixel; where they agree that is the
// answer, else six more around it give the penumbra's share. As in the
// shadow maps, the shade lightens with the distance to what casts it (by
// up to 65%, a smoothstep reaching it at 150 m).
layout(set = 4, binding = 0) uniform accelerationStructureEXT uScene;

// An instance's material (its custom index; vk::RayMaterial): where its
// triangles' vertices are, its texture and colour.
const uint kMatAlphaTest = 1u, kMatVertexColor = 2u, kMatLighting = 4u, kMatSky = 8u, kMatGlow = 16u,
           kMatNightLit = 32u;
struct RayMaterial {
    uvec2 vertices;   // vertex 0 (44 bytes each: normal at byte 12, colour 24, uv0 28)
    uvec2 indices;    // index 0
    uint indexBytes;  // 2, 4, or 0: not indexed
    uint firstVertex; // added to every index
    uint firstIndex;  // the geometry's first index (or vertex)
    uint texture;     // 0: white
    uint flags;
    float alphaRef;
    uint pad0, pad1;
    vec4 color;
};
layout(set = 4, binding = 1, std430) readonly buffer RayMaterialTable {
    RayMaterial entries[];
} uRayMaterials;
layout(set = 4, binding = 2) uniform sampler2D uRayTextures[];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RayWords {
    uint w[];
};

uint rayIndex(RayMaterial e, uint i) {
    if (e.indexBytes == 0u)
        return i;
    uint64_t base = packUint2x32(e.indices);
    if (e.indexBytes == 4u)
        return RayWords(base).w[i];
    uint64_t at = base + uint64_t(i) * 2ul;
    uint word = RayWords(at & ~3ul).w[0];
    return (at & 2ul) != 0ul ? word >> 16 : word & 0xFFFFu;
}
vec2 rayUv(RayMaterial e, uint v) {
    RayWords w = RayWords(packUint2x32(e.vertices) + uint64_t(v) * 44ul + 28ul);
    return vec2(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]));
}
vec4 rayVertexColor(RayMaterial e, uint v) {
    return unpackUnorm4x8(RayWords(packUint2x32(e.vertices) + uint64_t(v) * 44ul + 24ul).w[0]);
}
vec3 rayPosition(RayMaterial e, uint v) {
    RayWords w = RayWords(packUint2x32(e.vertices) + uint64_t(v) * 44ul);
    return vec3(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]), uintBitsToFloat(w.w[2]));
}
vec3 rayNormal(RayMaterial e, uint v) {
    RayWords w = RayWords(packUint2x32(e.vertices) + uint64_t(v) * 44ul + 12ul);
    return vec3(uintBitsToFloat(w.w[0]), uintBitsToFloat(w.w[1]), uintBitsToFloat(w.w[2]));
}
// The three vertices of triangle `prim` of entry `e`.
uvec3 rayTriangle(RayMaterial e, uint prim) {
    uint first = e.firstIndex + prim * 3u;
    return uvec3(rayIndex(e, first), rayIndex(e, first + 1u), rayIndex(e, first + 2u)) + uvec3(e.firstVertex);
}
// Whether an alpha tested candidate hit is solid there.
bool raySolid(RayMaterial e, uint prim, vec2 bc) {
    uvec3 v = rayTriangle(e, prim);
    vec2 uv = rayUv(e, v.x) * (1.0 - bc.x - bc.y) + rayUv(e, v.y) * bc.x + rayUv(e, v.z) * bc.y;
    return textureLod(uRayTextures[nonuniformEXT(e.texture)], uv, 0.0).a > e.alphaRef;
}

// How far along `dir` the ray from `o` meets the scene (< 0: nothing). An
// alpha tested triangle stops it only where its texture is solid.
float sunHit(vec3 o, vec3 dir) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, uScene, gl_RayFlagsTerminateOnFirstHitEXT, 0x01, o, 0.0, dir, 4000.0); // kRayShadow
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
            continue;
        RayMaterial e = uRayMaterials.entries[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)];
        if (raySolid(e, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)),
                     rayQueryGetIntersectionBarycentricsEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT
               ? -1.0
               : rayQueryGetIntersectionTEXT(q, true);
}

bool rayBlocked(vec3 o, vec3 dir, float len) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, uScene, gl_RayFlagsTerminateOnFirstHitEXT, 0x01, o, 0.0, dir, len); // kRayShadow
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
            continue;
        RayMaterial e = uRayMaterials.entries[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)];
        if (raySolid(e, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)),
                     rayQueryGetIntersectionBarycentricsEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

float sunRay(vec3 n) {
    const float kRadius = 0.01;
    vec3 l = -frame.lightDir[0].xyz;
    vec3 t = normalize(cross(l, abs(l.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(l, t);
    float dist = length(vWorldPos - frame.cameraPos.xyz);
    vec3 o = vWorldPos + n * (0.03 + dist * 0.0015) + l * 0.01;
    // Interleaved gradient noise: a turn per quad (its pixels share it).
    vec2 quadCoord = floor(gl_FragCoord.xy * 0.5);
    float turn = 6.2831853 * fract(52.9829189 * fract(dot(quadCoord, vec2(0.06711056, 0.00583715))));
    // Each pixel of the 2 x 2 quad traces one ray, at its quarter of the
    // disc, and the quad pools its four; where they disagree (a penumbra)
    // each traces one more on a spiral, eight in all. Quad uniform: the
    // second round's test is the same in its four pixels.
    uint lane = gl_SubgroupInvocationID & 3u;
    float hits = 0.0, far = 0.0, rays = 0.0;
    for (int round = 0; round < 2; ++round) {
        float k = float(lane) + float(round) * 4.0;
        float a = turn + k * 2.3999632;
        float r = sqrt((k + 0.5) / 8.0);
        float h = sunHit(o, normalize(l + (t * cos(a) + b * sin(a)) * (kRadius * r)));
        vec2 mine = h >= 0.0 ? vec2(1.0, h) : vec2(0.0);
        vec2 quad = mine + subgroupQuadSwapHorizontal(mine) + subgroupQuadSwapVertical(mine) +
                    subgroupQuadSwapDiagonal(mine);
        hits += quad.x;
        far += quad.y;
        rays += 4.0;
        if (round == 0 && (quad.x == 0.0 || quad.x == 4.0))
            break;
    }
    if (hits == 0.0)
        return 1.0;
    float lit = 1.0 - hits / rays;
    float fade = 0.65 * smoothstep(0.0, 1.0, (far / hits) * 0.02 / 3.0);
    return mix(lit, 1.0, fade);
}
#endif

#ifdef MM2_RAY_QUERY
// OpenMM2 ray traced reflections (FrameConstants::rayReflections): what a
// ray from `o` along `dir` sees, the nearest thing it meets (kRayReflect:
// the sky dome too) coloured as the game draws it, roughly: its texture
// (one level down), colour, vertex colour (the city's pre-lit geometry) or
// the frame's lights on its normal (the lit models), then the fog over the
// distance the light came. Nothing met: the fog's colour. The lights'
// glows it passes before that are added (up to four of them).
vec3 rayGlow(RayMaterial e, uint prim, vec2 bc) {
    uvec3 v = rayTriangle(e, prim);
    vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    vec4 c = e.color;
    if (e.texture != 0u)
        c *= textureLod(uRayTextures[nonuniformEXT(e.texture)],
                        rayUv(e, v.x) * w.x + rayUv(e, v.y) * w.y + rayUv(e, v.z) * w.z, 0.0);
    if ((e.flags & kMatVertexColor) != 0u)
        c *= rayVertexColor(e, v.x) * w.x + rayVertexColor(e, v.y) * w.y + rayVertexColor(e, v.z) * w.z;
    return c.rgb * c.a;
}

// The reflected rays' spread (radians between neighbouring pixels; 0: the
// hit's textures at mip level 1): far hits of spread rays read coarser mip
// levels, as a pixel's worth of them covers more of the texture.
float gRaySpread = 0.0;

vec3 rayReflect(vec3 o, vec3 dir, float reach) {
    rayQueryEXT q;
    // kRayReflect (not the sky dome: below). Back faces of one-sided draws
    // are not seen (their instances' culling is on).
    rayQueryInitializeEXT(q, uScene, gl_RayFlagsCullBackFacingTrianglesEXT, 0x02, o, 0.0, dir, reach);
    vec4 glows[4];
    int glowCount = 0;
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
            continue;
        RayMaterial e = uRayMaterials.entries[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)];
        uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
        vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, false);
        if ((e.flags & kMatGlow) != 0u) {
            if (glowCount < 4)
                glows[glowCount++] = vec4(rayGlow(e, prim, bc), rayQueryGetIntersectionTEXT(q, false));
            continue;
        }
        if (raySolid(e, prim, bc))
            rayQueryConfirmIntersectionEXT(q);
    }
    bool none = rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT;
    float far = none ? 1e9 : rayQueryGetIntersectionTEXT(q, true);
    vec3 glow = vec3(0.0);
    for (int i = 0; i < glowCount; ++i)
        if (glows[i].w < far)
            glow += glows[i].rgb;
    if (none) {
        // The sky: the dome along the ray's direction from the camera, as
        // the game draws it around the camera (kRaySky; it blocks nothing).
        rayQueryEXT sky;
        rayQueryInitializeEXT(sky, uScene, gl_RayFlagsOpaqueEXT, 0x04, frame.cameraPos.xyz, 0.0, dir, 100000.0);
        while (rayQueryProceedEXT(sky)) {
        }
        if (rayQueryGetIntersectionTypeEXT(sky, true) == gl_RayQueryCommittedIntersectionNoneEXT)
            return frame.fogColor.rgb + glow;
        RayMaterial e = uRayMaterials.entries[rayQueryGetIntersectionInstanceCustomIndexEXT(sky, true)];
        uvec3 v = rayTriangle(e, uint(rayQueryGetIntersectionPrimitiveIndexEXT(sky, true)));
        vec2 bc = rayQueryGetIntersectionBarycentricsEXT(sky, true);
        vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
        vec4 c = e.color;
        if (e.texture != 0u)
            c *= textureLod(uRayTextures[nonuniformEXT(e.texture)],
                            rayUv(e, v.x) * w.x + rayUv(e, v.y) * w.y + rayUv(e, v.z) * w.z, 1.0);
        if ((e.flags & kMatVertexColor) != 0u)
            c *= rayVertexColor(e, v.x) * w.x + rayVertexColor(e, v.y) * w.y + rayVertexColor(e, v.z) * w.z;
        return c.rgb + glow;
    }
    RayMaterial e = uRayMaterials.entries[rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)];
    uvec3 v = rayTriangle(e, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)));
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, true);
    vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    vec4 c = e.color;
    vec3 lit = vec3(0.0); // the glowing windows' colour (kMatNightLit)
    if (e.texture != 0u) {
        vec2 ua = rayUv(e, v.x), ub = rayUv(e, v.y), uc = rayUv(e, v.z);
        vec2 uv = ua * w.x + ub * w.y + uc * w.z;
        float lod = 1.0;
        if (gRaySpread > 0.0) {
            // The texture's texels per metre on this triangle, and the
            // footprint of the rays' spread where they hit.
            vec3 pa = rayPosition(e, v.x);
            float area = length(cross(rayPosition(e, v.y) - pa, rayPosition(e, v.z) - pa));
            vec2 du = ub - ua, dv = uc - ua;
            vec2 size = vec2(textureSize(uRayTextures[nonuniformEXT(e.texture)], 0));
            float texels = sqrt(abs(du.x * dv.y - du.y * dv.x) * size.x * size.y / max(area, 1e-8));
            float foot = gRaySpread * rayQueryGetIntersectionTEXT(q, true);
            lod = clamp(log2(max(foot * texels, 1.0)), 1.0, 8.0);
        }
        c *= textureLod(uRayTextures[nonuniformEXT(e.texture)], uv, lod);
        // The lit windows at most two levels coarser: averaged over a whole
        // facade they fall under windowLight's threshold and go out, where
        // the reflection of a lit city sparkles.
        if ((e.flags & kMatNightLit) != 0u && frame.cameraPos.w > 0.0)
            lit = windowLight(e.color.rgb *
                              textureLod(uRayTextures[nonuniformEXT(e.texture)], uv, min(lod, 2.0)).rgb);
    }
    vec3 albedo = c.rgb;
    if ((e.flags & kMatVertexColor) != 0u)
        c *= rayVertexColor(e, v.x) * w.x + rayVertexColor(e, v.y) * w.y + rayVertexColor(e, v.z) * w.z;
    if ((e.flags & kMatLighting) != 0u) {
        vec3 nObj = rayNormal(e, v.x) * w.x + rayNormal(e, v.y) * w.y + rayNormal(e, v.z) * w.z;
        mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(q, true);
        vec3 nw = normalize(mat3(toWorld) * nObj);
        if (dot(nw, dir) > 0.0)
            nw = -nw; // the side the ray came from
        vec3 light = frame.ambient.rgb;
        for (int i = 0; i < 3; ++i)
            light += frame.lightColor[i].rgb * max(dot(nw, -frame.lightDir[i].xyz), 0.0);
        c.rgb *= min(light, vec3(1.0));
    }
    // The night's lamps and headlights on what the ray sees (as mesh.frag
    // adds them: on the surface's own colour), with its face's normal.
    if (frame.lightInfo.x > 0.5) {
        mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(q, true);
        vec3 a = rayPosition(e, v.x), b = rayPosition(e, v.y), cc = rayPosition(e, v.z);
        vec3 fn = cross(mat3(toWorld) * (b - a), mat3(toWorld) * (cc - a));
        float fl = length(fn);
        if (fl > 1e-12) {
            fn /= fl;
            if (dot(fn, dir) > 0.0)
                fn = -fn;
            vec3 at = o + dir * rayQueryGetIntersectionTEXT(q, true);
            c.rgb += albedo * sceneLightsAt(at, fn, false);
        }
    }
    c.rgb = max(c.rgb, lit); // the glowing windows
    // The fog over the camera's distance to the reflector and on to the hit.
    float d = length(o - frame.cameraPos.xyz) + rayQueryGetIntersectionTEXT(q, true);
    float fog = 1.0;
    int mode = int(frame.fogParams.w + 0.5);
    if (mode == 1)
        fog = (frame.fogParams.y - d) / max(frame.fogParams.y - frame.fogParams.x, 1e-4);
    else if (mode == 2)
        fog = exp(-frame.fogParams.z * d);
    else if (mode == 3)
        fog = exp(-(frame.fogParams.z * d) * (frame.fogParams.z * d));
    return mix(frame.fogColor.rgb, c.rgb, clamp(fog, 0.0, 1.0)) + glow;
}
#endif

// The surface's own normal from the screen derivatives, facing the
// camera: many of the city's models carry up normals on their walls too.
vec3 surfaceNormal() {
    vec3 g = cross(dFdx(vWorldPos), dFdy(vWorldPos));
    float gl = length(g);
    if (!(gl > 1e-12))
        return vec3(0.0, 1.0, 0.0);
    g /= gl;
    return dot(g, frame.cameraPos.xyz - vWorldPos) < 0.0 ? -g : g;
}

#ifdef MM2_RAY_QUERY
// OpenMM2 wet roads, ray traced (frame.ground.x: the wetness, with ray
// traced reflections; else ssr.frag in screen space): ground facing up
// (its surface's normal, as ssr.frag rebuilds it, within ~30 degrees of
// up, not grass) mirrors the scene as
// ssr.frag's does, a ray along the view mirrored about the normal tilted
// by rain ripples (and between the puddles by the asphalt's bumps), mixed in by a Schlick Fresnel term and a world-space
// puddle pattern (puddles 1.0, wet sheen 0.35) and capped at 85%; the wet
// ground is a little darker. Unlike the screen's, the ray sees what is off
// it and behind the camera.
vec3 wetRoad(vec3 c, vec3 n) {
    float wet = frame.ground.x;
    float flat_ = smoothstep(0.82, 0.92, n.y);
    float green = smoothstep(0.02, 0.12, (c.g - max(c.r, c.b)) / max(c.g, 0.02));
    flat_ *= 1.0 - green; // 0: no reflection (but on to the quad's exchanges)
    vec2 xz = vWorldPos.xz;
    float puddle = smoothstep(0.42, 0.62, valueNoise(vec3(xz * 0.18, 0.0)) * 0.65 +
                                              valueNoise(vec3(xz * 0.61, 5.0)) * 0.35);
    float reflectivity = mix(0.35, 1.0, puddle) * wet * flat_;
    float time = frame.ground.w;
    vec2 ripple = vec2(valueNoise(vec3(xz * 3.1 + time * 1.7, 0.0)), valueNoise(vec3(xz.yx * 3.3 - time * 1.9, 9.0))) - 0.5;
    // Between the puddles the asphalt is uneven: a bumpier mirror (two
    // octaves, 40 and 15 cm), which smears what it reflects (a roofline
    // mirrored in a flat sheen was a hard straight line across the road).
    vec2 rough = vec2(valueNoise(vec3(xz * 2.5, 21.0)) + 0.5 * valueNoise(vec3(xz * 6.7, 33.0)),
                      valueNoise(vec3(xz.yx * 2.5, 45.0)) + 0.5 * valueNoise(vec3(xz.yx * 6.7, 57.0))) / 1.5 - 0.5;
    // Far away many bumps fall in one pixel: the mirror grows rougher and
    // weaker with distance (from 30 m, fully at 120 m), or the far road
    // was a sheet of glass at the grazing angles Fresnel favours.
    float far = smoothstep(30.0, 120.0, length(vWorldPos - frame.cameraPos.xyz));
    vec3 mirror = normalize(n + vec3(ripple.x, 0.0, ripple.y) * mix(0.025, 0.01, puddle) +
                            vec3(rough.x, 0.0, rough.y) * (0.12 * (1.0 - puddle) + 0.08 * far));
    vec3 view = normalize(vWorldPos - frame.cameraPos.xyz);
    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(-view, mirror), 0.0, 1.0), 5.0);
    float amount = clamp(reflectivity * fresnel * 1.6, 0.0, mix(0.85, 0.3, far));
    c *= mix(1.0, 0.82, wet * flat_);
    // Out to 300 m (the wet ground's reflection of what is farther is faded
    // and fogged; a ray that far is the sky): the long rays cost the most.
    // Two diagonal pixels of the 2 x 2 quad trace (where any of the quad's
    // reflection would show), the other two take their mean: half the rays
    // (the quad's exchanges run alike in its four pixels).
    float need = amount >= 0.02 ? 1.0 : 0.0;
    need = max(max(need, subgroupQuadSwapHorizontal(need)), max(subgroupQuadSwapVertical(need), subgroupQuadSwapDiagonal(need)));
    if (need == 0.0)
        return c; // too faint to see anywhere in the quad: no ray
    uint lane = gl_SubgroupInvocationID & 3u;
    vec3 refl = vec3(0.0);
    if (lane == 0u || lane == 3u)
        refl = rayReflect(vWorldPos + n * 0.05, reflect(view, mirror), 300.0);
    vec3 r0 = subgroupQuadBroadcast(refl, 0u), r3 = subgroupQuadBroadcast(refl, 3u);
    refl = lane == 0u ? r0 : lane == 3u ? r3 : 0.5 * (r0 + r3);
    return mix(c, refl, amount);
}
#endif

// OpenMM2 water (FrameConstants::water in fogColor.w; not in MM2): the
// normal of a few deep-water waves (each moving at its own speed,
// sqrt(g k), from its own direction) and fine ripples, each faded out where
// a pixel spans a good part of its length (no shimmer far out). `foot`: the
// pixel's size on the water (m).
vec3 waterNormal(vec2 xz, float t, float foot) {
    // direction (x, z), length (m), height (m)
    const vec4 waves[5] = vec4[5](vec4(0.96, 0.28, 13.0, 0.09), vec4(0.48, -0.88, 7.1, 0.05),
                                  vec4(-0.62, 0.78, 4.3, 0.03), vec4(0.12, 0.99, 2.3, 0.014),
                                  vec4(-0.94, -0.34, 1.3, 0.007));
    vec2 g = vec2(0.0);
    for (int i = 0; i < 5; ++i) {
        float len = waves[i].z;
        float k = 6.2831853 / len;
        float fade = 1.0 - smoothstep(0.15 * len, 0.5 * len, foot);
        // In groups: each wave's height wanders over some ten of its
        // lengths (drifting with it), so no regular stripes run out to sea.
        float group = valueNoise(vec3(xz / (len * 9.0) + waves[i].xy * (t * 0.05), float(i) * 3.7));
        g += waves[i].xy * (waves[i].w * k * cos(dot(waves[i].xy, xz) * k - sqrt(9.81 * k) * t) * fade *
                            (0.25 + 1.5 * group * group));
    }
    float fine = 1.0 - smoothstep(0.08, 0.3, foot);
    g += (vec2(valueNoise(vec3(xz * 2.3 + t * 0.9, 0.0)), valueNoise(vec3(xz.yx * 2.1 - t * 0.8, 7.0))) - 0.5) *
         (0.12 * fine);
    return normalize(vec3(-g.x, 1.0, -g.y));
}

// The water's colour over `c` (its texture, baked light and shade): a
// Fresnel mirror of what is above (the sky, or traced) over the darker
// water, plus the key light's glint. `lit`: how much the sun reaches it.
vec3 waterSurface(vec3 c, float lit) {
    vec3 toCam = frame.cameraPos.xyz - vWorldPos;
    float dist = max(length(toCam), 1e-3);
    vec3 view = -toCam / dist;
    float foot = max(length(fwidth(vWorldPos.xz)), 1e-4);
    vec3 n = waterNormal(vWorldPos.xz, frame.ground.w, foot);
    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(-view, n), 0.0, 1.0), 5.0);
    vec3 r = reflect(view, n);
    if (r.y < 0.02)
        r = normalize(vec3(r.x, 0.02, r.z)); // a wave's back: the horizon
    vec3 refl;
#ifdef MM2_RAY_QUERY
    if (frame.fogColor.w > 1.5) {
        // Traced along a calmer normal (the waves' tilt at 40%): at full
        // tilt the rays scattered over far facades' windows, a grainy
        // mirror where a real one blurs. As wetRoad, two diagonal pixels of
        // the quad trace, the other two take their mean.
        vec3 calm = normalize(vec3(n.x * 0.4, n.y, n.z * 0.4));
        vec3 rt = reflect(view, calm);
        if (rt.y < 0.02)
            rt = normalize(vec3(rt.x, 0.02, rt.z));
        gRaySpread = length(fwidth(rt));
        uint lane = gl_SubgroupInvocationID & 3u;
        vec3 t = vec3(0.0);
        if (lane == 0u || lane == 3u)
            t = rayReflect(vWorldPos + vec3(0.0, 0.05, 0.0), rt, 2000.0);
        gRaySpread = 0.0;
        vec3 r0 = subgroupQuadBroadcast(t, 0u), r3 = subgroupQuadBroadcast(t, 3u);
        refl = lane == 0u ? r0 : lane == 3u ? r3 : 0.5 * (r0 + r3);
    } else
#endif
    {
        // Untraced: the sky only, no brighter than the haze at its horizon,
        // and a weaker mirror (the city, which it cannot show, would stand
        // in much of it).
        vec3 zenith = frame.fogColor.rgb * vec3(0.55, 0.72, 1.05);
        refl = mix(frame.fogColor.rgb * 0.85, zenith, sqrt(r.y));
        fresnel *= 0.5;
    }
    float amount = clamp(0.06 + fresnel, 0.0, 0.85);
    vec3 col = mix(c, refl, amount);
    // The glint: a sharp highlight (the sun's disc spread by the waves).
    float s = max(dot(r, -frame.lightDir[0].xyz), 0.0);
    float glint = pow(s, 800.0) * 6.0 + pow(s, 90.0) * 0.25;
    return col + frame.lightColor[0].rgb * (glint * lit);
}

// The world around the player's car along `d` from `o`: traced
// (FrameConstants::rayReflections) or from the probe.
vec3 envColor(vec3 o, vec3 d) {
#ifdef MM2_RAY_QUERY
    if (frame.lightInfo.z > 0.5)
        return rayReflect(o, d, 100000.0);
#endif
    return probeColor(d);
}

float sunVisibility(vec3 n, float facing) {
#ifdef MM2_RAY_QUERY
    if (frame.lightInfo.y > 0.5)
        return sunRay(n);
#endif
    int count = int(frame.shadowParams.y + 0.5);
    for (int i = 0; i < count; ++i) {
        vec4 c = shadowCoord(i, n, facing);
        if (c.w >= 0.97)
            continue;
        float lit = shadowLit(i, c.xy, c.z);
        // Into the next over the outer quarter; the last fades out over
        // its outer half, so the shadows thin out into the distance.
        float edge = smoothstep(i + 1 < count ? 0.75 : 0.5, 0.97, c.w);
        if (edge > 0.0) {
            float next = 1.0;
            if (i + 1 < count) {
                vec4 c2 = shadowCoord(i + 1, n, facing);
                if (c2.w < 0.97)
                    next = shadowLit(i + 1, c2.xy, c2.z);
            }
            lit = mix(lit, next, edge);
        }
        return lit;
    }
    return 1.0;
}

void main() {
    vec4 c = vColor;
    // OpenMM2 enhanced lighting (kPixelLighting; not in MM2). The same
    // Direct3D 7 formula as mesh.vert, per pixel and with the normal
    // renormalised, plus a Blinn-Phong highlight from the key light added
    // after the texture.
    bool pixel = (draw.flags & kPixelLighting) != 0u;
    bool probe = (draw.flags & kEnvProbe) != 0u;
    vec3 n = vec3(0.0, 1.0, 0.0);
    vec3 toEye = vec3(0.0, 0.0, 1.0);
    if (pixel || probe) {
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
    vec4 tex0 = vec4(1.0);
    if ((draw.flags & kTexture0) != 0u) {
        tex0 = texture(uTexture0, vUv0);
        c *= tex0;
    }
    // OpenMM2 clear coat: a Fresnel mirror of the sky over the paint and a
    // sharp sun glint (with the highlight, so the shadows take it).
    vec3 coat = vec3(0.0);
    float gloss = pixel ? draw.emissive.w : 0.0;
    if (gloss > 0.0) {
        float facing = 1.0 - max(dot(n, toEye), 0.0);
        float f2 = facing * facing;
        float fresnel = 0.04 + 0.96 * f2 * f2 * facing;
        vec3 r = reflect(-toEye, n);
        float k = min(gloss * (0.1 + 0.9 * fresnel), 1.0);
        coat = (probe ? envColor(vWorldPos + n * 0.05, r) : coatSky(r)) * k;
        c.rgb *= 1.0 - k; // the coat's reflection, not the paint, at glancing angles
        float glint = max(dot(r, -frame.lightDir[0].xyz), 0.0);
        highlight += frame.lightColor[0].rgb * (gloss * (0.6 + fresnel) * 2.5 * pow(glint, 900.0));
    }
    // OpenMM2 grass: patches, the shells' blades and the soil between them.
    if ((draw.flags & kGrass) != 0u && frame.ground.y > 0.0) {
        float green = grassGreen(c.rgb);
        if ((draw.flags & kGrassShell) != 0u) {
            vec4 blade = grassShell(c.rgb, green); // discards outside the blades first
            c = vec4(blade.rgb, c.a * blade.a);
        } else
            c.rgb *= 1.0 - 0.15 * green * grassGrow(vWorldPos);
        c.rgb = grassTint(c.rgb, green);
    }
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
        vec4 t1 = probe ? vec4(envColor(vWorldPos + n * 0.05, reflect(-toEye, n)), 1.0) : texture(uTexture1, uv1);
        if ((draw.flags & kTex1Add) != 0u)
            c.rgb = min(c.rgb + t1.rgb, vec3(1.0));
        else if ((draw.flags & kTex1Mod2x) != 0u)
            c.rgb = min(c.rgb * t1.rgb * 2.0, vec3(1.0));
        else if ((draw.flags & kTex1Blend) != 0u)
            c.rgb = mix(c.rgb, t1.rgb, t1.a);
        else
            c *= t1;
    }
    // D3DCMP_GREATEREQUAL against the reference value. With ray queries at
    // the end: the quad's pixels share their rays, and all four must run.
    bool cut = (draw.flags & kAlphaTest) != 0u && c.a < draw.alphaRef;
#ifndef MM2_RAY_QUERY
    if (cut)
        discard;
#endif
    // OpenMM2 sun shadows: darken what the sun does not reach (the game's
    // colours are mostly pre-lit, so the whole colour; the highlight too).
    // The darkening is weighted by how much the surface faces the sun:
    // a side facing away got no sunlight in the game's own lighting either,
    // and this keeps grazing surfaces from flickering in and out of shadow.
    // The grass shells take the shade from the grass under them (it is in
    // the alpha, BlendMode::Shaded), which keeps it for them: grass draws
    // write it, 1 in the sun.
    bool shell = (draw.flags & kGrassShell) != 0u;
    float shade = 1.0;
    if (frame.shadowParams.x > 0.0 && (draw.flags & kReceiveShadow) != 0u && !shell) {
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
#ifdef MM2_RAY_QUERY
        // Every pixel: the quad shares the sun's rays (sunRay).
        float sun = sunVisibility(sn, facing);
#else
        float sun = weight > 0.0 ? sunVisibility(sn, facing) : 1.0;
#endif
        shade = 1.0 - frame.shadowParams.x * weight * (1.0 - sun);
        c.rgb *= shade;
        highlight *= sun;
    }
    // OpenMM2 night lighting: the scene lights on the surface's own colour
    // (texture x material; the vertex colours carry the game's baked light).
    bool solid = (draw.flags & kReceiveShadow) != 0u;
    if (solid && frame.lightInfo.x > 0.5) {
        vec3 albedo = tex0.rgb * draw.color.rgb;
        c.rgb += albedo * sceneLights(lightingNormal(pixel, n));
    }
    if ((draw.flags & kNightLit) != 0u && frame.cameraPos.w > 0.0)
        c.rgb = max(c.rgb, windowLight(tex0.rgb * draw.color.rgb));
    bool water = (draw.flags & kWater) != 0u && frame.fogColor.w > 0.5;
    if (water)
        c.rgb = waterSurface(c.rgb, shade);
    c.rgb = min(c.rgb + highlight + coat, vec3(1.0));
    if (solid)
        c.rgb = applyHaze(c.rgb);
    c.rgb = mix(frame.fogColor.rgb, c.rgb, vFog);
#ifdef MM2_RAY_QUERY
    if (frame.ground.x > 0.0 && solid && !pixel && !probe && (draw.flags & kGrassShell) == 0u && !water)
        c.rgb = wetRoad(c.rgb, surfaceNormal());
#endif
#ifdef MM2_RAY_QUERY
    if (cut)
        discard;
#endif
    if (shell)
        c.rgb *= c.a; // premultiplied (BlendMode::Shaded)
    else if ((draw.flags & kGrass) != 0u)
        c.a = shade;  // for the shells over it
    oColor = c;
}
