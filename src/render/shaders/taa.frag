// OpenMM2 temporal anti-aliasing and upscaling (render::PostProcess::taa;
// not in MM2). The scene is drawn each frame with its projection shifted by
// a different sub-pixel amount (Device::taaJitter); this pass, drawn at the
// window's resolution, blends the frame into the history of the last ones
// (the previous output, uTexture2): a pixel's view-space position (from the
// distance image) goes through the previous frame's camera and projection
// to where it was, and the history is read there. Kept from straying by the
// current frame's 3 x 3 neighbourhood (a clip towards its mean in YCoCg,
// within its spread), so moving cars and newly revealed places do not
// ghost. Points near the camera and the car it follows move with the
// camera: their history is where they are now. A drawn-in rectangle (the
// mirror, with its own camera) keeps the current frame only.

TEXTURE0 uTexture0; // the scene so far (scene resolution, jittered)
TEXTURE1 uTexture1; // distances (r; scene resolution, jittered)
TEXTURE2 uHistory;  // the last output (window resolution)

DRAW_BLOCK {
    mat4 toPrevious; // this frame's view space -> the previous frame's clip space
    vec4 params;     // proj[0][0], proj[1][1], far, history weight (0: none, a cut)
    vec4 jitter;     // this frame's offset in the scene's uv (x, y), still within (m), unused
    vec4 focus;      // the followed car in view space, its radius (0: none)
    vec4 still;      // a rectangle in uv (x0, y0, x1, y1) with no history; x1 <= x0: none
} taa;

layout(location = 0) out vec4 oColor;

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

vec3 toYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom from five bilinear taps (the corners dropped): the scene at
// a sharper filter than bilinear, for the upscaling.
vec3 sampleSharp(sampler2D t, vec2 uv) {
    vec2 size = vec2(textureSize(t, 0));
    vec2 p = uv * size;
    vec2 c = floor(p - 0.5) + 0.5;
    vec2 f = p - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c - 1.0) / size, t3 = (c + 2.0) / size, t12 = (c + w2 / w12) / size;
    vec3 r = textureLod(t, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y) +
             textureLod(t, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y) +
             textureLod(t, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y) +
             textureLod(t, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y) +
             textureLod(t, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / w, vec3(0.0));
}

void main() {
    vec2 outSize = vec2(textureSize(uHistory, 0));
    vec2 uv = gl_FragCoord.xy / outSize;
    // Where this pixel is in the jittered frame.
    vec2 cur = uv + taa.jitter.xy;
    vec3 color = sampleSharp(uTexture0, cur);
    vec4 base = textureLod(uTexture0, cur, 0.0);
    bool still = taa.still.z > taa.still.x && uv.x >= taa.still.x && uv.x < taa.still.z && uv.y >= taa.still.y &&
                 uv.y < taa.still.w;
    if (taa.params.w <= 0.0 || still) {
        oColor = vec4(color, base.a);
        return;
    }
    // The neighbourhood, in the scene's pixels, and its nearest distance
    // (the thin edges in front reproject with what is in front).
    vec2 px = 1.0 / vec2(textureSize(uTexture0, 0));
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float t = taa.params.z;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 q = cur + vec2(float(x), float(y)) * px;
            vec3 s = toYCoCg(textureLod(uTexture0, q, 0.0).rgb);
            m1 += s;
            m2 += s * s;
            t = min(t, textureLod(uTexture1, q, 0.0).r);
        }
    m1 /= 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
    vec3 lo = m1 - 1.25 * sigma, hi = m1 + 1.25 * sigma;

    // Where it was. Near the camera, or on and around the car it follows,
    // it moves with the camera.
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, ndcY(uv));
    vec3 p = vec3(ndc.x * t / taa.params.x, ndc.y * t / taa.params.y, -t);
    vec2 prevUv = uv;
    bool withCamera = t < taa.jitter.z ||
                      (taa.focus.w > 0.0 && distance(p, taa.focus.xyz) < taa.focus.w);
    if (!withCamera && t < taa.params.z * 0.999) {
        vec4 prev = taa.toPrevious * vec4(p, 1.0);
        if (prev.w > 0.01)
            prevUv = vec2(prev.x / prev.w * 0.5 + 0.5, uvY(prev.y / prev.w));
    } else if (!withCamera) {
        // The sky: only the camera's turn (its view direction, far away).
        vec4 prev = taa.toPrevious * vec4(p * (1.0 / max(t, 1e-3)) * 1e4, 1.0);
        if (prev.w > 0.01)
            prevUv = vec2(prev.x / prev.w * 0.5 + 0.5, uvY(prev.y / prev.w));
    }
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0) {
        oColor = vec4(color, base.a);
        return;
    }
    vec3 hist = toYCoCg(sampleSharp(uHistory, prevUv));
    // Clip towards the mean, not merely clamp: no colour casts.
    vec3 d = hist - m1;
    vec3 range = max(hi - m1, vec3(1e-4));
    float k = max(max(abs(d.x) / range.x, abs(d.y) / range.y), abs(d.z) / range.z);
    if (k > 1.0)
        hist = m1 + d / k;
    // Less history the farther the pixel moved (sub-pixel blur builds up).
    float moved = length((prevUv - uv) * outSize);
    float weight = taa.params.w * mix(1.0, 0.8, clamp(moved / 8.0, 0.0, 1.0));
    vec3 c = mix(toYCoCg(color), hist, weight);
    oColor = vec4(max(fromYCoCg(c), vec3(0.0)), base.a);
}
