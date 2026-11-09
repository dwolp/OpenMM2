// OpenMM2 ambient occlusion, step 3 (render::PostProcess::ao; not in MM2):
// blurs the occlusion (ao.frag) over 4x4 pixels, the size of its noise
// pattern, weighting each by how close its distance is to this pixel's so
// the shading stops at edges, and darkens the scene with it.

TEXTURE0 uTexture0; // occlusion (r) and distance (g)
TEXTURE1 uTexture1; // the scene

DRAW_BLOCK {
    vec4 params; // strength (0..1), unused, 1 / width, 1 / height
} blur;

layout(location = 0) out vec4 oColor;

void main() {
    vec2 px = blur.params.zw;
    vec2 uv = gl_FragCoord.xy * px;
    float t0 = textureLod(uTexture0, uv, 0.0).g;
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            vec2 s = textureLod(uTexture0, uv + (vec2(x, y) + 0.5) * px, 0.0).rg;
            float w = exp(-abs(s.y - t0) / max(t0 * 0.02, 0.05));
            sum += s.x * w;
            weight += w;
        }
    }
    float open = weight > 0.0 ? sum / weight : 1.0;
    // A power curve: partly enclosed places darken a little more than
    // linearly (strength 0.6: open^1.72, 1: open^2.2), and from 0.6 down
    // the exponent falls to 0, so a low strength is a light touch (it used
    // to stay at open^1 or more however low the slider went).
    float s = blur.params.x;
    float shade = pow(max(open, 0.0), s < 0.6 ? s * (1.72 / 0.6) : 1.0 + 1.2 * s);
    vec4 c = textureLod(uTexture1, uv, 0.0);
    oColor = vec4(c.rgb * shade, c.a);
}
