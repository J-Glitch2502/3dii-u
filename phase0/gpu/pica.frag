#version 450
// One shader that does what the PICA200's fixed pipeline does per pixel: two textures, per-pixel lighting through lookup
// tables, six combiner stages chosen at run time, fog and alpha test. A real port can use cheaper specialized versions,
// so this is deliberately the expensive case.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vView;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D tex0;
layout(binding = 1) uniform sampler2D tex1;
layout(binding = 2) uniform sampler2D lut;

layout(binding = 0, std140) uniform Tev {
    ivec4 stage[6];  // x, y, z: sources a, b, c; w: operation
    vec4 konst[6];
    vec4 light0;
    vec4 light1;
    vec4 fogColor;
};

vec4 combine(int op, vec4 a, vec4 b, vec4 c)
{
    if (op == 0) return a;
    if (op == 1) return a * b;
    if (op == 2) return min(a + b, vec4(1.0));
    if (op == 3) return clamp(a + b - 0.5, 0.0, 1.0);
    if (op == 4) return mix(b, a, c);
    if (op == 5) return max(a - b, vec4(0.0));
    if (op == 6) return min(a * b + c, vec4(1.0));
    return min((a + b) * c, vec4(1.0));
}

void main()
{
    vec4 t0 = texture(tex0, vUv);
    vec4 t1 = texture(tex1, vUv * 3.0);
    vec3 n = normalize(vNormal);
    vec3 v = normalize(vView);

    vec3 primary = vec3(0.1);
    vec3 secondary = vec3(0.0);
    for (int i = 0; i < 2; i++) {
        vec3 l = normalize(i == 0 ? light0.xyz : light1.xyz);
        vec3 h = normalize(l + v);
        float diffuse = max(dot(n, l), 0.0);
        float spec = texture(lut, vec2(max(dot(n, h), 0.0), 0.25)).r;
        primary += diffuse * vColor.rgb;
        secondary += spec * vec3(0.3);
    }

    vec4 sources[6];
    sources[0] = vec4(primary, 1.0);
    sources[1] = vec4(secondary, 1.0);
    sources[2] = t0;
    sources[3] = t1;
    sources[4] = vColor;
    sources[5] = vec4(0.0);  // the previous stage's result

    for (int i = 0; i < 6; i++) {
        ivec4 s = stage[i] & ivec4(7);
        vec4 a = s.x > 5 ? konst[i] : sources[s.x];
        vec4 b = s.y > 5 ? konst[i] : sources[s.y];
        vec4 c = s.z > 5 ? konst[i] : sources[s.z];
        sources[5] = clamp(combine(s.w, a, b, c), 0.0, 1.0);
    }

    float fog = texture(lut, vec2(clamp(length(vView) * 0.5, 0.0, 1.0), 0.75)).r;
    vec4 color = mix(fogColor, sources[5], fog);
    if (color.a < 0.004) discard;
    FragColor = vec4(color.rgb, 1.0);
}
