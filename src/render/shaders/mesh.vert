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
    vec4 shadowParams;   // strength (0 = none), texel (world), size (texels), softness
    vec4 surface;        // OpenMM2 surfaces: detail, relief, smooth filtering (0/1), distance (m)
    vec4 ground;         // OpenMM2: surface variation, grass height (m, 0 = no grass), grass distance (m), time (s)
    vec4 haze;           // OpenMM2 distance haze: density (1/m), height falloff (1/m), sun glow, most
    vec4 lightInfo;      // OpenMM2 night lighting: light count
    vec4 lightPos[48];   // position, range
    vec4 lightSpot[48];  // direction, cos outer (< -1: a point light)
    vec4 lightTint[48];   // colour, cos inner
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
const uint kGrassShell = 32768u;

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
out float gl_ClipDistance[1]; // OpenMM2 grass shells: clipped at the grass distance

void main() {
    vec4 worldPos = draw.world * vec4(aPosition, 1.0);
    // OpenMM2 grass: a shell layer is the ground lifted by its share
    // (draw.specular) of the grass height. The whole height: fading it here
    // by distance would fade it at the triangle's corners, and the fields'
    // triangles are hundreds of metres across (mesh.frag shrinks the blades
    // per pixel instead). Clipped past the grass distance in view depth, a
    // plane the rasteriser cuts exactly, so a field's far end is not drawn
    // 16 times over.
    bool shell = (draw.flags & kGrassShell) != 0u;
    if (shell)
        worldPos.y += draw.specular * frame.ground.y;
    vec4 viewPos = frame.view * worldPos;
    gl_ClipDistance[0] = shell ? frame.ground.z + viewPos.z : 1.0;
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
