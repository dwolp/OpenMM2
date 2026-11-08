// OpenMM2 ambient occlusion, step 2 (render::PostProcess::ao; not in MM2):
// screen-space ambient occlusion from the distance image (depthcopy.frag).
// Each pixel's view-space position and normal are rebuilt from the
// distances; 12 points in the hemisphere around the normal, turned by a
// per-pixel angle, count as occluded when the scene in front of them is
// closer to the camera. A range check keeps far-away geometry from
// darkening edges. Output: occlusion (1 = open) and the distance, which
// the blur (aoblur.frag) uses to keep edges sharp.

TEXTURE0 uTexture0; // distances (r)

DRAW_BLOCK {
    vec4 params; // proj[0][0], proj[1][1], radius (m), unused
    vec4 extra;  // 1 / width, 1 / height, far, fade distance (m)
} ao;

layout(location = 0) out vec4 oColor;

vec2 texelSize() { return ao.extra.xy; }

float distanceAt(vec2 uv) { return textureLod(uTexture0, uv, 0.0).r; }

// View space: x right, y up (or down: the result is mirrored consistently),
// the camera looking down -z.
vec3 viewPos(vec2 uv, float t) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * t / ao.params.x, ndc.y * t / ao.params.y, -t);
}
vec3 viewPosAt(vec2 uv) { return viewPos(uv, distanceAt(uv)); }

vec2 project(vec3 p) {
    vec2 ndc = vec2(p.x * ao.params.x, p.y * ao.params.y) / -p.z;
    return ndc * 0.5 + 0.5;
}

void main() {
    vec2 uv = gl_FragCoord.xy * texelSize();
    float t = distanceAt(uv);
    float far = ao.extra.z;
    if (t >= far * 0.999 || t > ao.extra.w) { // the sky, or too far to matter
        oColor = vec4(1.0, t, 0.0, 1.0);
        return;
    }
    vec3 p = viewPos(uv, t);

    // The normal from the neighbour on the side whose distance is closer, so
    // silhouettes do not bend it.
    vec2 px = texelSize();
    vec3 r = viewPosAt(uv + vec2(px.x, 0.0)) - p, l = p - viewPosAt(uv - vec2(px.x, 0.0));
    vec3 u = viewPosAt(uv + vec2(0.0, px.y)) - p, d = p - viewPosAt(uv - vec2(0.0, px.y));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(u.z) < abs(d.z) ? u : d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, -p) < 0.0)
        n = -n; // face the camera whatever the image's row order

    // A per-pixel rotation (interleaved gradient noise) the blur removes.
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float angle = noise * 6.2831853;
    vec3 helper = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, n));
    vec3 bitangent = cross(n, tangent);
    float c = cos(angle), s = sin(angle);
    vec3 tr = tangent * c + bitangent * s, br = bitangent * c - tangent * s;

    const int kSamples = 12;
    float radius = ao.params.z;
    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // A golden-angle spiral over the hemisphere, denser near the point.
        float fi = float(i) + 0.5;
        float z = 1.0 - fi / float(kSamples);        // height above the surface
        float rr = sqrt(max(1.0 - z * z, 0.0));
        float phi = fi * 2.3999632;
        float scale = mix(0.15, 1.0, (fi / float(kSamples)) * (fi / float(kSamples)));
        vec3 k = (tr * (cos(phi) * rr) + br * (sin(phi) * rr) + n * z) * (radius * scale);
        vec3 q = p + k;
        vec2 quv = project(q);
        if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0)
            continue;
        float sceneZ = -distanceAt(quv);
        float range = smoothstep(0.0, 1.0, radius / max(abs(p.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= q.z + 0.02 * t / 10.0 + 0.02 ? 1.0 : 0.0) * range;
    }
    float open = 1.0 - occlusion / float(kSamples);
    // Fade out towards the fade distance, where the samples grow too sparse.
    open = mix(open, 1.0, smoothstep(ao.extra.w * 0.6, ao.extra.w, t));
    oColor = vec4(open, t, 0.0, 1.0);
}
