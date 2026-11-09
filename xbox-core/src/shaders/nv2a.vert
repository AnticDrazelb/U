#version 450
// Screen-space vertices from the CPU side (x, y in surface pixels, z in depth units, w): back to clip
// space, so the GPU interpolates perspective-correctly as the software rasterizer does.
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aDiffuse;
layout(location = 2) in vec4 aSpecular;
layout(location = 3) in vec4 aFog;
layout(location = 4) in vec4 aTex0;
layout(location = 5) in vec4 aTex1;
layout(location = 6) in vec4 aTex2;
layout(location = 7) in vec4 aTex3;
layout(location = 0) out vec4 vDiffuse;
layout(location = 1) out vec4 vSpecular;
layout(location = 2) out vec4 vFog;
layout(location = 3) out vec4 vTex0;
layout(location = 4) out vec4 vTex1;
layout(location = 5) out vec4 vTex2;
layout(location = 6) out vec4 vTex3;
layout(push_constant) uniform Screen { vec4 scale; } pc; // 2 / width, 2 / height, 1 / z max
void main()
{
    const float w = aPos.w;
    const vec3 ndc = vec3(aPos.x * pc.scale.x - 1.0, aPos.y * pc.scale.y - 1.0, aPos.z * pc.scale.z);
    gl_Position = vec4(ndc * w, w);
    vDiffuse = aDiffuse;
    vSpecular = aSpecular;
    vFog = aFog;
    vTex0 = aTex0;
    vTex1 = aTex1;
    vTex2 = aTex2;
    vTex3 = aTex3;
}
