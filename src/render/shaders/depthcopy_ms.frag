// depthcopy.frag for a multisampled scene depth (Vulkan with MSAA): the
// first sample of each pixel.

#ifdef MM2_VULKAN
layout(set = 1, binding = 0) uniform sampler2DMS uTexture0; // the scene depth
#else
uniform sampler2DMS uTexture0;
#endif

DRAW_BLOCK {
    vec4 params; // A (proj[2][2]), B (proj[3][2]), far, unused
} copy;

layout(location = 0) out vec4 oColor;

void main() {
    float d = texelFetch(uTexture0, ivec2(gl_FragCoord.xy), 0).r;
    float t = copy.params.y / (d + copy.params.x);
    oColor = vec4(min(t, copy.params.z), 0.0, 0.0, 1.0);
}
