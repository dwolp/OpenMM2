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
    vec4 shadowParams;   // strength (0 = none), texel (world), size (texels), unused
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
    vec2 t = uv * size - 0.5;
    ivec2 base = ivec2(floor(t)) - 2;
    vec2 f = fract(t);
    float wx[6] = float[6](1.0 - f.x, 1.0, 1.0, 1.0, 1.0, f.x);
    float wy[6] = float[6](1.0 - f.y, 1.0, 1.0, 1.0, 1.0, f.y);
    ivec2 top = ivec2(int(size) - 1);
    float lit = 0.0;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            float d = texelFetch(uShadow, clamp(base + ivec2(x, y), ivec2(0), top), 0).r;
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
        c *= texture(uTexture0, vUv0);
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
        float len = length(vWorldNormal);
        vec3 sn = len > 1e-5 ? vWorldNormal / len : vec3(0.0);
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
