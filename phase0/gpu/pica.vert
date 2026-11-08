#version 450
// The kind of work a PICA200 vertex program does: a matrix transform, a normal and a view vector.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vView;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;

// Built in, not uniform, so the test draws correctly even if uniform byte order were wrong.
const mat4 world = mat4(0.98, 0.17, 0.0, 0.0,
                        -0.17, 0.98, 0.0, 0.0,
                        0.0, 0.0, 1.0, 0.0,
                        0.0, 0.0, 0.0, 1.0);

void main()
{
    vec4 p = world * vec4(aPos.xy * 1.2, aPos.z, 1.0);
    gl_Position = vec4(p.xy, aPos.z, 1.0);
    vNormal = mat3(world) * aNormal;
    vView = vec3(0.0, 0.0, 1.0) - p.xyz;
    vUv = aUv;
    vColor = vec4(aNormal * 0.5 + 0.5, 1.0);
}
