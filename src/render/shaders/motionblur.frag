// OpenMM2 motion blur (render::PostProcess::motionBlur; not in MM2): camera
// motion blur. Each pixel's view-space position (from the distance image)
// goes through the previous frame's camera and projection; the change in
// its screen position, scaled to a fixed exposure, is the direction and
// length of a 12-tap blur. Points close to the camera (the player's own
// car, the cockpit) move with it and stay sharp, and so does the ground
// around the car the camera follows (blur.focus), where its sun shadow
// falls: blurred as the road, the shadow smeared as the car turned.

TEXTURE0 uTexture0; // the scene so far
TEXTURE1 uTexture1; // distances (r)

DRAW_BLOCK {
    mat4 toPrevious; // this frame's view space -> the previous frame's clip space
    vec4 params;     // proj[0][0], proj[1][1], far, exposure scale (strength x (1/60 s) / frame time)
    vec4 limits;     // longest blur (uv), sharp within (m), fully blurred from (m), unused
    vec4 focus;      // the followed car in view space; sharp within w (m), fully blurred from 2w; w 0: none
} blur;

layout(location = 0) out vec4 oColor;

float ndcY(vec2 uv) {
#ifdef MM2_VULKAN
    return 1.0 - 2.0 * uv.y; // row 0 at the top
#else
    return 2.0 * uv.y - 1.0;
#endif
}
// The view-space point at `uv`, `t` metres away.
vec3 viewPos(vec2 uv, float t) {
    return vec3((uv.x * 2.0 - 1.0) * t / blur.params.x, ndcY(uv) * t / blur.params.y, -t);
}
// How much the point at `uv`, `t` metres away, blurs: none near the camera
// or near the followed car, fully farther from both.
float blurAmount(vec2 uv, float t) {
    float a = smoothstep(blur.limits.y, blur.limits.z, t);
    if (blur.focus.w > 0.0 && a > 0.0)
        a *= smoothstep(blur.focus.w, 2.0 * blur.focus.w, distance(viewPos(uv, t), blur.focus.xyz));
    return a;
}

float uvY(float y) {
#ifdef MM2_VULKAN
    return 0.5 - 0.5 * y;
#else
    return 0.5 + 0.5 * y;
#endif
}

void main() {
    vec2 size = vec2(textureSize(uTexture0, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec4 base = textureLod(uTexture0, uv, 0.0);
    oColor = base;
    float t = min(textureLod(uTexture1, uv, 0.0).r, blur.params.z);
    float near = blurAmount(uv, t);
    if (near <= 0.0 || blur.params.w <= 0.0)
        return;
    vec3 p = viewPos(uv, t);
    vec4 prev = blur.toPrevious * vec4(p, 1.0);
    if (prev.w <= 0.01)
        return;
    vec2 prevUv = vec2(prev.x / prev.w * 0.5 + 0.5, uvY(prev.y / prev.w));
    vec2 velocity = (uv - prevUv) * blur.params.w * near;
    float len = length(velocity);
    if (len < 0.5 / size.y)
        return;
    velocity *= min(1.0, blur.limits.x / len);

    // Centred taps along the motion; the sky and far points blur fully, near
    // ones fade in.
    const int kTaps = 12;
    vec3 sum = base.rgb;
    float weight = 1.0;
    for (int i = 0; i < kTaps; ++i) {
        float f = (float(i) + 0.5) / float(kTaps) - 0.5;
        vec2 q = clamp(uv + velocity * f, vec2(0.0), vec2(1.0));
        // Do not smear the sharp foreground (the car, its shadow) over its
        // background.
        float w = blurAmount(q, min(textureLod(uTexture1, q, 0.0).r, blur.params.z));
        sum += textureLod(uTexture0, q, 0.0).rgb * w;
        weight += w;
    }
    oColor = vec4(sum / weight, base.a);
}
