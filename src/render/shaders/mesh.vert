// World geometry and models: fixed-function style vertex lighting and fog.

FRAME_BLOCK {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPos;
    vec4 fogColor;
    vec4 fogParams; // start, end, density, mode (0 none, 1 linear, 2 exp, 3 exp2)
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

const uint kLighting = 16u;
const uint kVertexColor = 32u;
const uint kEnvMap1 = 64u;
const uint kFog = 8u;
const uint kPixelLighting = 1024u;

ATTR(0) vec3 aPosition;
ATTR(1) vec3 aNormal;
ATTR(2) vec4 aColor;
ATTR(3) vec2 aUv0;
ATTR(4) vec2 aUv1;

OUT(0) vec4 vColor;
OUT(1) vec2 vUv0;
OUT(2) vec2 vUv1;
OUT(3) float vFog;
OUT(4) vec3 vWorldNormal; // kPixelLighting
OUT(5) vec3 vWorldPos;    // kPixelLighting

void main() {
    vec4 worldPos = draw.world * vec4(aPosition, 1.0);
    vec4 viewPos = frame.view * worldPos;
    gl_Position = clipFixup(frame.proj * viewPos);

    vec4 color = draw.color;
    if ((draw.flags & kVertexColor) != 0u)
        color *= aColor;
    // Direct3D 7's fixed function takes normals through the inverse
    // transpose of the world matrix and, with D3DRENDERSTATE_NORMALIZENORMALS
    // off (MM2 never sets it), does not renormalise them: a scaled instance
    // is lit in proportion to 1 / its scale (inferred from Direct3D).
    vec3 worldNormal = transpose(inverse(mat3(draw.world))) * aNormal;
    vWorldNormal = worldNormal;
    vWorldPos = worldPos.xyz;
    // OpenMM2 enhanced lighting: the fragment shader lights the material colour.
    if ((draw.flags & kLighting) != 0u && (draw.flags & kPixelLighting) == 0u) {
        vec3 light = frame.ambient.rgb;
        for (int i = 0; i < 3; ++i)
            light += frame.lightColor[i].rgb * max(dot(worldNormal, -frame.lightDir[i].xyz), 0.0);
        // Direct3D 7: emissive + (ambient + lights) x material, clamped to
        // 0..1 per vertex (the material's ambient is its diffuse).
        color.rgb = clamp(draw.emissive.rgb + color.rgb * light, 0.0, 1.0);
    }
    vColor = color;
    vUv0 = aUv0;
    if ((draw.flags & kEnvMap1) != 0u) {
        // modShader::BeginEnvMap: D3DTSS_TCI_CAMERASPACENORMAL taken back to
        // world space by the camera matrix, then u = 0.5 + 0.5 x and
        // v = 0.5 - 0.5 y of the world-space normal.
        vUv1 = vec2(worldNormal.x, -worldNormal.y) * 0.5 + 0.5;
    } else {
        vUv1 = aUv1;
    }

    float fog = 1.0;
    int mode = int(frame.fogParams.w + 0.5);
    if ((draw.flags & kFog) != 0u && mode != 0) {
        float d = -viewPos.z;
        if (mode == 1)
            fog = (frame.fogParams.y - d) / max(frame.fogParams.y - frame.fogParams.x, 1e-4);
        else if (mode == 2)
            fog = exp(-frame.fogParams.z * d);
        else
            fog = exp(-(frame.fogParams.z * d) * (frame.fogParams.z * d));
        fog = clamp(fog, 0.0, 1.0);
    }
    vFog = fog;
}
