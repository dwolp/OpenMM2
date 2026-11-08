// One pass of OpenMM2's bloom chain (render::PostProcess::bloom; not in
// MM2), drawn with composite.vert's full-screen triangle into a level of the
// chain. Down passes halve the image with a 13-tap filter (the bright pass
// first keeps only what is brighter than the threshold, with a soft knee);
// up passes blur the next smaller level with a 3x3 tent and mix in this
// level's down image, so the glow ends both wide and smooth.

TEXTURE0 uTexture0; // the source: the scene or the previous level
TEXTURE1 uTexture1; // up passes: the next smaller level's up image

DRAW_BLOCK {
    vec4 params; // mode (0 bright pass + down, 1 down, 2 up), threshold
    vec4 texel;  // 1 / size of uTexture0 (xy), of uTexture1 (zw)
} bloom;

IN(0) vec2 vUv;

layout(location = 0) out vec4 oColor;

vec3 down13(vec2 uv, vec2 t) {
    // Thirteen bilinear taps: a centre 2x2 box of weight 0.5 and four
    // corner boxes of 0.125 (overlapping), which keeps small bright spots
    // from flickering as the camera moves.
    vec3 a = texture(uTexture0, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 b = texture(uTexture0, uv + t * vec2(0.0, -2.0)).rgb;
    vec3 c = texture(uTexture0, uv + t * vec2(2.0, -2.0)).rgb;
    vec3 d = texture(uTexture0, uv + t * vec2(-2.0, 0.0)).rgb;
    vec3 e = texture(uTexture0, uv).rgb;
    vec3 f = texture(uTexture0, uv + t * vec2(2.0, 0.0)).rgb;
    vec3 g = texture(uTexture0, uv + t * vec2(-2.0, 2.0)).rgb;
    vec3 h = texture(uTexture0, uv + t * vec2(0.0, 2.0)).rgb;
    vec3 i = texture(uTexture0, uv + t * vec2(2.0, 2.0)).rgb;
    vec3 j = texture(uTexture0, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 k = texture(uTexture0, uv + t * vec2(1.0, -1.0)).rgb;
    vec3 l = texture(uTexture0, uv + t * vec2(-1.0, 1.0)).rgb;
    vec3 m = texture(uTexture0, uv + t * vec2(1.0, 1.0)).rgb;
    return (j + k + l + m) * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + e * 0.125;
}

vec3 brightPass(vec3 c, float threshold) {
    float l = max(c.r, max(c.g, c.b));
    float knee = threshold * 0.5;
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float w = max(soft, l - threshold) / max(l, 1e-4);
    return c * w;
}

vec3 tent(vec2 uv, vec2 t) {
    vec3 s = texture(uTexture1, uv).rgb * 4.0;
    s += (texture(uTexture1, uv + vec2(t.x, 0.0)).rgb + texture(uTexture1, uv - vec2(t.x, 0.0)).rgb +
          texture(uTexture1, uv + vec2(0.0, t.y)).rgb + texture(uTexture1, uv - vec2(0.0, t.y)).rgb) * 2.0;
    s += texture(uTexture1, uv + t).rgb + texture(uTexture1, uv - t).rgb +
         texture(uTexture1, uv + vec2(t.x, -t.y)).rgb + texture(uTexture1, uv + vec2(-t.x, t.y)).rgb;
    return s / 16.0;
}

void main() {
    int mode = int(bloom.params.x + 0.5);
    vec3 c;
    if (mode == 2) {
        // 70% of the wider glow below and 30% of this level: the wide levels
        // still count at the top of the chain, and the mix never leaves the
        // 0..1 range of an 8-bit target.
        c = mix(texture(uTexture0, vUv).rgb, tent(vUv, bloom.texel.zw), 0.7);
    } else {
        c = down13(vUv, bloom.texel.xy);
        if (mode == 0)
            c = brightPass(c, bloom.params.y);
    }
    oColor = vec4(c, 1.0);
}
