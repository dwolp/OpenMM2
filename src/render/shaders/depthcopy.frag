// OpenMM2 ambient occlusion, step 1 (render::PostProcess::ao; not in MM2):
// the scene's depth (0..1, Direct3D style) as the distance in front of the
// camera, t = B / (d + A) for the main camera's projection. One texel per
// pixel; depthcopy_ms.frag is the same for a multisampled depth (sample 0).

TEXTURE0 uTexture0; // the scene depth

DRAW_BLOCK {
    vec4 params; // A (proj[2][2]), B (proj[3][2]), far, unused
} copy;

layout(location = 0) out vec4 oColor;

void main() {
    float d = texelFetch(uTexture0, ivec2(gl_FragCoord.xy), 0).r;
    float t = copy.params.y / (d + copy.params.x);
    oColor = vec4(min(t, copy.params.z), 0.0, 0.0, 1.0);
}
