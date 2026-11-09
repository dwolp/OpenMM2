// OpenMM2 sun shafts and glare (render::PostProcess::shafts; not in MM2).
// Light shafts in screen space: from each pixel, 48 steps towards the sun's
// position on screen gather the sky's colour wherever the distance image
// shows open sky, decaying along the way, so beams stream past buildings,
// trees and signals. A glow around the sun, scaled by how much of the
// sun's disc is open sky, adds the glare. Colour and strength follow the
// key light; with the sun behind the camera there is nothing.

TEXTURE0 uTexture0; // the scene so far
TEXTURE1 uTexture1; // distances (r)

DRAW_BLOCK {
    vec4 sun;   // the sun's screen position (ndc x, y), on-screen weight (0..1), strength
    vec4 color; // the key light's colour (rgb), far plane
} shafts;

layout(location = 0) out vec4 oColor;

float uvY(float y) {
#ifdef MM2_VULKAN
    return 0.5 - 0.5 * y; // row 0 at the top
#else
    return 0.5 + 0.5 * y;
#endif
}

float sky(vec2 uv) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 0.0;
    return textureLod(uTexture1, uv, 0.0).r >= shafts.color.w * 0.999 ? 1.0 : 0.0;
}

void main() {
    vec2 size = vec2(textureSize(uTexture0, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec4 base = textureLod(uTexture0, uv, 0.0);
    oColor = base;
    float weight = shafts.sun.z * shafts.sun.w;
    if (weight <= 0.0)
        return;
    vec2 sunUv = vec2(shafts.sun.x * 0.5 + 0.5, uvY(shafts.sun.y));
    vec2 aspect = vec2(size.x / size.y, 1.0);
    float d = length((uv - sunUv) * aspect);
    // Shafts fade away from the sun so the whole screen is not washed; past
    // ~2 screen heights from it nothing would show, so no march there.
    // (Jittering the march's start per pixel was tried for fewer steps: the
    // scattered reads made it slower than 48 coherent ones.)
    float falloff = exp(-d * 2.2);
    if (falloff < 0.012)
        return;

    // The shafts: march towards the sun, gathering open sky.
    const int kSteps = 48;
    vec2 delta = (sunUv - uv) * (0.92 / float(kSteps));
    vec2 p = uv;
    float illumination = 1.0;
    vec3 gathered = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        p += delta;
        vec2 q = clamp(p, vec2(0.0), vec2(1.0));
        // Only the sky's brightest part feeds the shafts (the glow near the sun).
        vec3 s = max(textureLod(uTexture0, q, 0.0).rgb - 0.55, 0.0) * (sky(p) / 0.45);
        gathered += s * illumination;
        illumination *= 0.965;
    }
    gathered *= 1.0 / float(kSteps);

    // How much of the sun is open sky: 12 points on and around its disc.
    float open = sky(sunUv);
    for (int i = 0; i < 11; ++i) {
        float a = float(i) * 0.5711987;
        float r = 0.012 + 0.018 * float(i % 3);
        open += sky(sunUv + vec2(cos(a), sin(a)) * vec2(r * size.y / size.x, r));
    }
    open /= 12.0;

    float glare = (exp(-d * 16.0) * 0.45 + exp(-d * 5.0) * 0.12) * open;
    vec3 light = shafts.color.rgb * (gathered * 0.9 * falloff + glare);
    oColor = vec4(base.rgb + light * weight, base.a);
}
