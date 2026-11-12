// Writes GLSL for the PICA200's fragment pipeline and vertex programs, compiled ahead of time by tools/shadergen_main.cpp.
// A port of gen_fs()/gen_lighting() in runtime/gpu/metal.mm: the same structure, but the combiners work in floats, which Latte
// runs far faster than its integer operations.
#include "shadergen.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace gpu {
namespace {
struct Gen {
    std::string s;
    void f(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        s += b;
    }
};

const char *src_expr(u32 i) {
    switch (i) {
    case 0: return "prim";
    case 1: return "fpc";
    case 2: return "fsc";
    case 3: return "tx0";
    case 4: return "tx1";
    case 5: return "tx2";
    case 6: return "tx3";
    case 13: return "buf";
    case 14: return "K";
    case 15: return "outc";
    default: return "vec4(0.0)";
    }
}

std::string color_mod(u32 m, const char *v) {
    char b[160];
    switch (m) {
    case 1: snprintf(b, sizeof b, "(1.0 - (%s).rgb)", v); break;
    case 2: snprintf(b, sizeof b, "(%s).aaa", v); break;
    case 3: snprintf(b, sizeof b, "(1.0 - (%s).aaa)", v); break;
    case 4: snprintf(b, sizeof b, "(%s).rrr", v); break;
    case 5: snprintf(b, sizeof b, "(1.0 - (%s).rrr)", v); break;
    case 8: snprintf(b, sizeof b, "(%s).ggg", v); break;
    case 9: snprintf(b, sizeof b, "(1.0 - (%s).ggg)", v); break;
    case 12: snprintf(b, sizeof b, "(%s).bbb", v); break;
    case 13: snprintf(b, sizeof b, "(1.0 - (%s).bbb)", v); break;
    default: snprintf(b, sizeof b, "(%s).rgb", v); break;
    }
    return b;
}
std::string alpha_mod(u32 m, const char *v) {
    static const char *ch[8] = {"a", "a", "r", "r", "g", "g", "b", "b"};
    char b[96];
    snprintf(b, sizeof b, (m & 1) ? "(1.0 - (%s).%s)" : "(%s).%s", v, ch[m & 7]);
    return b;
}
std::string combine(u32 op, const char *a, const char *b, const char *c, bool vec) {
    char o[256];
    const char *one = vec ? "vec3(1.0)" : "1.0", *zero = vec ? "vec3(0.0)" : "0.0";
    switch (op) {
    case 1: snprintf(o, sizeof o, "(%s * %s)", a, b); break;
    case 2: snprintf(o, sizeof o, "min(%s + %s, %s)", a, b, one); break;
    case 3: snprintf(o, sizeof o, "clamp(%s + %s - 0.50196, 0.0, 1.0)", a, b); break;
    case 4: snprintf(o, sizeof o, "(%s * %s + %s * (1.0 - %s))", a, c, b, c); break;
    case 5: snprintf(o, sizeof o, "max(%s - %s, %s)", a, b, zero); break;
    case 8: snprintf(o, sizeof o, "min(%s * %s + %s, %s)", a, b, c, one); break;
    case 9: snprintf(o, sizeof o, "(min(%s + %s, %s) * %s)", a, b, one, c); break;
    default: snprintf(o, sizeof o, "%s", a); break;
    }
    return o;
}
const char *cmp_expr(u32 func) {
    switch (func) {
    case 0: return "false";
    case 1: return "true";
    case 2: return "A == B";
    case 3: return "A != B";
    case 4: return "A < B";
    case 5: return "A <= B";
    case 6: return "A > B";
    default: return "A >= B";
    }
}

// q and -q are the same rotation, but interpolating between them passes through zero, which ruins lighting: gray patches on any model whose vertices
// disagree. A 3DS flips them per triangle, and a GX2 vertex shader can't see the triangle. The rotated axes don't change with the sign, so those are what's interpolated.
const char *const frame_outputs =
    "    { float d = length(q); q = d > 0.0 ? q * (1.0 / d) : vec4(0.0, 0.0, 0.0, 1.0);\n"
    "      vQuat = vec4(2.0 * (q.x * q.z + q.w * q.y), 2.0 * (q.y * q.z - q.w * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y), 0.0);\n"
    "      vTan = vec4(1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.w * q.z), 2.0 * (q.x * q.z - q.w * q.y), 0.0); }\n";

void gen_lighting(Gen &g, const LightingState &l) {
    g.f("    vec4 shadow = vec4(1.0);\n");
    if (l.shadow_enable) {
        g.f("    shadow = tx%u;\n", l.shadow_sel & 3);
        if (l.shadow_invert) g.f("    shadow = 1.0 - shadow;\n");
    }
    g.f("    vec3 sn = vec3(0.0, 0.0, 1.0), st = vec3(1.0, 0.0, 0.0);\n");
    if (l.bump_mode == 1 || l.bump_mode == 2) {
        g.f("    { vec3 p = tx%u.rgb * 2.00787 - 1.0;\n", l.bump_sel & 3);
        if (l.bump_mode == 1) {
            if (l.bump_renorm) g.f("      p.z = sqrt(max(1.0 - (p.x * p.x + p.y * p.y), 0.0));\n");
            g.f("      sn = p; }\n");
        } else {
            g.f("      st = p; }\n");
        }
    }
    // the surface's frame, from the vertex shader as vectors (see frame_outputs)
    g.f("    vec3 ln = vQuat.xyz; { float d = length(ln); ln = d > 0.0 ? ln * (1.0 / d) : vec3(0.0, 0.0, 1.0); }\n");
    if (l.bump_mode == 1 || l.bump_mode == 2 || l.config == 8)
        g.f("    vec3 lt = vTan.xyz; { float d = length(lt); lt = d > 0.0 ? lt * (1.0 / d) : vec3(1.0, 0.0, 0.0); }\n"
            "    vec3 lb = cross(ln, lt);\n"
            "    vec3 normal = lt * sn.x + lb * sn.y + ln * sn.z;\n"
            "    vec3 tangent = lt * st.x + lb * st.y + ln * st.z;\n");
    else
        g.f("    vec3 normal = ln;\n"
            "    vec3 tangent = vec3(1.0, 0.0, 0.0);\n");
    g.f("    vec3 view = vView.xyz;\n"
        "    float vl = length(view); vec3 nv = vl > 0.0 ? view * (1.0 / vl) : view;\n"
        "    vec4 dsum = vec4(0.0, 0.0, 0.0, 1.0), ssum = vec4(0.0, 0.0, 0.0, 1.0);\n");
    const char *sc[7] = {"uLscale0.x", "uLscale0.y", "uLscale0.z", "uLscale0.w", "uLscale1.x", "uLscale1.y", "uLscale1.z"};
    const LutSel *sels[7] = {&l.d0, &l.d1, &l.sp, &l.fr, &l.rb, &l.rg, &l.rr};
    for (u32 li = 0; li < l.num_lights; li++) {
        const LightState &lt = l.lights[li];
        g.f("    { // light %u (#%u)\n", li, lt.num);
        g.f("      vec3 lv = uLights[%u].pos.xyz%s;\n", li, lt.directional ? "" : " + view");
        g.f("      float len = length(lv); if (len > 0.0) lv = lv * (1.0 / len);\n"
            "      vec3 hv = nv + lv;\n"
            "      float dist = 1.0;\n");
        if (lt.dist_atten)
            g.f("      { float loc = clamp(uLights[%u].att.x * len + uLights[%u].att.y, 0.0, 1.0); float fl = floor(loc * 256.0);\n"
                "        int idx = int(clamp(fl, 0.0, 255.0)); vec2 e = lut_entry(texelFetch(uLut, ivec2(idx, %u), 0));\n"
                "        dist = e.x + e.y * (loc * 256.0 - float(idx)); }\n",
                li, li, 16 + lt.num);
        g.f("      float hl = length(hv); vec3 nh = hl > 0.0 ? hv * (1.0 / hl) : hv;\n"
            "      float lin0 = dot(normal, nh), lin1 = dot(nv, nh), lin2 = dot(normal, nv), lin3 = dot(lv, normal), lin4 = dot(lv, uLights[%u].spot.xyz);\n",
            li);
        if (l.config == 8)
            g.f("      float lin5 = dot(nh - normal * lin0, tangent);\n");
        else
            g.f("      float lin5 = 0.0;\n");
        auto lv = [&](int si, u32 sampler) {
            const LutSel &s = *sels[si];
            char b[200];
            snprintf(b, sizeof b, "(%s * lutv(%u, %s, %s, %s))", sc[si], sampler,
                     s.input < 6 ? ("lin" + std::to_string(s.input)).c_str() : "0.0", s.abs ? "true" : "false",
                     lt.two_sided ? "true" : "false");
            return std::string(b);
        };
        g.f("      float spot = %s;\n", lt.spot_atten && l.sp.enabled ? lv(2, 8 + lt.num).c_str() : "1.0");
        g.f("      float d0 = %s;\n", l.d0.enabled ? lv(0, 0).c_str() : "1.0");
        g.f("      vec3 refl; refl.x = %s;\n", l.rr.enabled ? lv(6, 6).c_str() : "1.0");
        g.f("      refl.y = %s;\n", l.rg.enabled ? lv(5, 5).c_str() : "refl.x");
        g.f("      refl.z = %s;\n", l.rb.enabled ? lv(4, 4).c_str() : "refl.x");
        g.f("      float d1 = %s;\n", l.d1.enabled ? lv(1, 1).c_str() : "1.0");
        if (li == l.num_lights - 1 && l.fr.enabled) {
            g.f("      { float fr = %s;\n", lv(3, 3).c_str());
            if (l.primary_alpha) g.f("        dsum.a = fr;\n");
            if (l.secondary_alpha) g.f("        ssum.a = fr;\n");
            g.f("      }\n");
        }
        g.f("      float dp = lin3; dp = %s;\n", lt.two_sided ? "abs(dp)" : "(dp > 0.0 ? dp : 0.0)");
        g.f("      float clamp_h = %s;\n", l.clamp_highlights ? "(dp == 0.0 ? 0.0 : 1.0)" : "1.0");
        if (lt.geo0 || lt.geo1) g.f("      float h2 = dot(hv, hv); float geo = h2 == 0.0 ? 0.0 : min(dp / h2, 1.0);\n");
        g.f("      float g0 = %s, g1 = %s;\n", lt.geo0 ? "geo" : "1.0", lt.geo1 ? "geo" : "1.0");
        const bool shp = l.shadow_primary && lt.shadow, shs = l.shadow_secondary && lt.shadow;
        g.f("      float att = dist * spot;\n"
            "      vec3 s0 = d0 * uLights[%u].spec0.rgb * g0;\n"
            "      vec3 s1 = d1 * refl * uLights[%u].spec1.rgb * g1;\n"
            "      dsum.rgb += (uLights[%u].diffuse.rgb * dp%s + uLights[%u].ambient.rgb) * att;\n"
            "      ssum.rgb += (s0 + s1) * clamp_h * att%s;\n"
            "    }\n",
            li, li, li, shp ? " * shadow.rgb" : "", li, shs ? " * shadow.rgb" : "");
    }
    if (l.shadow_alpha) {
        if (l.primary_alpha) g.f("    dsum.a *= shadow.a;\n");
        if (l.secondary_alpha) g.f("    ssum.a *= shadow.a;\n");
    }
    g.f("    dsum.rgb += uAmbient.rgb;\n"
        "    fpc = clamp(dsum, 0.0, 1.0); fsc = clamp(ssum, 0.0, 1.0);\n");
}
}  // namespace

std::string gen_fs_glsl(const DrawState &ds) {
    Gen g;
    g.s = "#version 450\n";
    const bool shade = ds.need_shade, need_zw = ds.wbuffer || (shade && ds.fog);
    if (shade) {
        g.f("layout(location = 0) in vec4 vColor;\n"
            "layout(location = 1) in vec4 vTc01;\n"
            "layout(location = 2) in vec4 vTc2w;\n");
        if (ds.lighting)
            g.f("layout(location = 3) in vec4 vQuat;\n"
                "layout(location = 4) in vec4 vView;\n");
        if (ds.lighting && (ds.light.bump_mode == 1 || ds.light.bump_mode == 2 || ds.light.config == 8)) g.f("layout(location = 6) in vec4 vTan;\n");
    }
    if (need_zw) g.f("layout(location = 5) in vec2 vZw;\n");
    g.f("layout(location = 0) out vec4 FragColor;\n"
        "struct Light { vec4 spec0, spec1, diffuse, ambient, pos, spot, att; };\n"
        "layout(binding = 0, std140) uniform Uni {\n"
        "    vec4 uDepth;  // scale, offset, alpha reference (0..255), render target scale\n"
        "    ivec4 uScissor;\n"
        "    vec4 uKonst[6];\n"
        "    vec4 uTevInit[3];\n"
        "    vec4 uFogColor;\n"
        "    vec4 uBorder[3];\n"
        "    vec4 uShadow;\n"
        "    vec4 uLscale0, uLscale1, uAmbient;\n"
        "    Light uLights[8];\n");
    const bool proctex = shade && uses_proctex(ds);
    if (proctex) g.f("    vec4 uPt[5];  // PsProctex in gx2_backend.cpp\n");
    g.f("};\n");
    bool cube = false;
    for (int i = 0; i < 3; i++)
        if (shade && ds.need_tex[i] && !ds.tex[i].null_tex) {
            g.f("layout(binding = %d) uniform sampler2D t%d;\n", i, i);
            if (i == 0 && (ds.tex[0].type == 1 || ds.tex[0].type == 4)) cube = true;
        }
    if (cube)
        for (int f = 1; f < 6; f++) g.f("layout(binding = %d) uniform sampler2D f%d;\n", 2 + f, f);
    if (shade && ds.lighting) {
        g.f("layout(binding = 8) uniform sampler2D uLut;\n"
            "vec2 lut_entry(vec4 t) { return vec2(t.r * 0.99609375 + t.g * 0.00389105, (t.b * 0.99609375 + t.a * 0.00389105) * 2.0 - 1.0); }\n"
            "float lutv(int s, float r, bool absm, bool two) {\n"
            "    int idx; float delta;\n"
            "    if (absm) { r = two ? abs(r) : max(r, 0.0); float fl = floor(r * 256.0); idx = int(clamp(fl, 0.0, 255.0)); delta = r * 256.0 - float(idx); }\n"
            "    else { float fl = floor(r * 128.0); int si = int(clamp(fl, -128.0, 127.0)); delta = r * 128.0 - float(si); idx = si < 0 ? si + 256 : si; }\n"
            "    vec2 e = lut_entry(texelFetch(uLut, ivec2(idx, s), 0)); return e.x + e.y * delta; }\n");
    }
    if (shade && ds.fog) {
        g.f("layout(binding = 9) uniform sampler2D uFog;\n");
        if (!ds.lighting)
            g.f("vec2 lut_entry(vec4 t) { return vec2(t.r * 0.99609375 + t.g * 0.00389105, (t.b * 0.99609375 + t.a * 0.00389105) * 2.0 - 1.0); }\n");
    }
    // sample_proctex() in runtime/gpu/fragment.cpp. Settings come from uniforms, since the keys in pipelines.bin can't name them.
    if (proctex)
        g.s += ("layout(binding = 10) uniform sampler2D uPtLut;\n"
            "layout(binding = 11) uniform sampler2D uPtRand;\n"
            "float pt_map(int row, float c) {\n"
            "    c *= 128.0; float i = clamp(floor(c), 0.0, 127.0); vec4 t = texelFetch(uPtLut, ivec2(int(i), row), 0);\n"
            "    return t.r * 0.99609375 + t.g * 0.00389105 + (c - i) * ((t.b * 0.99609375 + t.a * 0.00389105) * 2.0 - 1.0); }\n"
            "float pt_rand(float x, float y) { return texelFetch(uPtRand, ivec2(int(mod(x, 144.0)), int(mod(y, 144.0))), 0).r * 2.0 - 1.0; }\n"
            "float pt_wrap(float c, float m) {\n"
            "    if (m == 0.0) return c > 1.0 ? 0.0 : c;\n"
            "    if (m == 2.0) return fract(c);\n"
            "    if (m == 3.0) return mod(floor(c), 2.0) == 0.0 ? fract(c) : 1.0 - fract(c);\n"
            "    if (m == 4.0) return c <= 0.5 ? 0.0 : 1.0;\n"
            "    return min(c, 1.0); }\n"
            "float pt_shift(float c, float m, float wrap) {\n"
            "    if (m != 1.0 && m != 2.0) return 0.0;\n"
            "    return (wrap == 3.0 ? 1.0 : 0.5) * mod(floor((floor(c) + (m == 2.0 ? 1.0 : 0.0)) / 2.0), 2.0); }\n"
            "float pt_comb(float u, float v, float m, int row) {\n"
            "    float f = 0.0, l = sqrt(u * u + v * v);\n"
            "    if (m == 0.0) f = u; else if (m == 1.0) f = u * u; else if (m == 2.0) f = v; else if (m == 3.0) f = v * v;\n"
            "    else if (m == 4.0) f = (u + v) * 0.5; else if (m == 5.0) f = (u * u + v * v) * 0.5; else if (m == 6.0) f = min(l, 1.0);\n"
            "    else if (m == 7.0) f = min(u, v); else if (m == 8.0) f = max(u, v); else if (m == 9.0) f = min(((u + v) * 0.5 + l) * 0.5, 1.0);\n"
            "    return pt_map(row, f); }\n"
            "vec4 pt_sample(vec2 uv) {\n"
            "    float u = abs(uv.x), v = abs(uv.y);\n"
            "    float us = pt_shift(v, uPt[3].z, uPt[3].x), vs = pt_shift(u, uPt[3].w, uPt[3].y);\n"
            "    if (uPt[0].x != 0.0 || uPt[0].y != 0.0) {\n"
            "        float x = uPt[1].x * abs(u + uPt[0].z), y = uPt[1].y * abs(v + uPt[0].w);\n"
            "        float xi = floor(x), yi = floor(y), xf = x - xi, yf = y - yi;\n"
            "        float g0 = pt_rand(xi, yi) * (xf + yf), g1 = pt_rand(xi + 1.0, yi) * (xf + yf - 1.0);\n"
            "        float g2 = pt_rand(xi, yi + 1.0) * (xf + yf - 1.0), g3 = pt_rand(xi + 1.0, yi + 1.0) * (xf + yf - 2.0);\n"
            "        float xn = pt_map(0, xf), yn = pt_map(0, yf);\n"
            "        float n = mix(mix(g0, g1, xn), mix(g2, g3, xn), yn);\n"
            "        u = abs(u + n * uPt[0].x); v = abs(v + n * uPt[0].y);\n"
            "    }\n"
            "    u = pt_wrap(u + us, uPt[3].x); v = pt_wrap(v + vs, uPt[3].y);\n"
            "    float i = clamp(uPt[1].z + pt_comb(u, v, uPt[4].x, 1) * uPt[1].w, 0.0, 255.0), fi = floor(i);\n"
            "    vec4 c;\n"
            "    if (uPt[4].w != 0.0) c = texelFetch(uPtLut, ivec2(int(fi), 3), 0) + (i - fi) * (texelFetch(uPtLut, ivec2(int(fi), 4), 0) - 0.50196) * 2.0;\n"
            "    else c = texelFetch(uPtLut, ivec2(int(floor(i + 0.5)), 3), 0);\n"
            "    if (uPt[4].z != 0.0) c.a = pt_comb(u, v, uPt[4].y, 2);\n"
            "    return clamp(c, 0.0, 1.0); }\n");
    g.f("void main() {\n");
    if (need_zw) {
        g.f("    float zd = vZw.x / vZw.y * uDepth.x + uDepth.y;\n");
        if (ds.wbuffer) g.f("    zd *= vZw.y;\n");
        g.f("    zd = clamp(zd, 0.0, 1.0);\n");
        if (ds.wbuffer) g.f("    gl_FragDepth = zd;\n");
    }
    if (ds.scissor_mode == 1)
        g.f("    { ivec2 pp = ivec2(floor(gl_FragCoord.xy / uDepth.w));\n"
            "      if (pp.x >= uScissor.x && pp.x < uScissor.z && pp.y >= uScissor.y && pp.y < uScissor.w) discard; }\n");
    if (!shade) {
        g.f("    FragColor = vec4(0.0);\n}\n");  // colour writes are masked off
        return g.s;
    }
    g.f("    vec4 prim = clamp(vColor, 0.0, 1.0);\n"
        "    vec2 c0 = vTc01.xy, c1 = vTc01.zw, c2 = vTc2w.xy; float tw = vTc2w.z;\n"
        "    vec4 tx0 = vec4(0.0), tx1 = vec4(0.0), tx2 = vec4(0.0), tx3 = vec4(0.0);\n"
        "    vec4 fpc = vec4(0.0), fsc = vec4(0.0);\n");
    for (int i = 0; i < 3; i++) {
        if (!ds.need_tex[i]) continue;
        const TexUnitState &t = ds.tex[i];
        if (t.null_tex) {
            g.f("    tx%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
            continue;
        }
        const u32 type = i == 0 ? t.type : 0;
        if (type == 1 || type == 4) {
            // cube map: face selection as in the software renderer
            g.f("    { float cu = c0.x, cv = c0.y, cw = tw; float au = abs(cu), av = abs(cv), aw = abs(cw);\n"
                "      float x, y, z; int face;\n"
                "      if (au > av && au > aw) { face = cu > 0.0 ? 0 : 1; y = cu > 0.0 ? -cv : cv; x = -cw; z = cu; }\n"
                "      else if (av > aw) { face = cv > 0.0 ? 2 : 3; x = cv > 0.0 ? cu : -cu; y = cw; z = cv; }\n"
                "      else { face = cw > 0.0 ? 4 : 5; y = cw > 0.0 ? -cv : cv; x = cu; z = cw; }\n"
                "      vec2 uv = vec2(x / z * 0.5 + 0.5, y / z * 0.5 + 0.5);\n"
                "      if (face == 0) tx0 = textureLod(t0, uv, 0.0); else if (face == 1) tx0 = textureLod(f1, uv, 0.0);\n"
                "      else if (face == 2) tx0 = textureLod(f2, uv, 0.0); else if (face == 3) tx0 = textureLod(f3, uv, 0.0);\n"
                "      else if (face == 4) tx0 = textureLod(f4, uv, 0.0); else tx0 = textureLod(f5, uv, 0.0); }\n");
            continue;
        }
        if (type == 2) {
            g.f("    { vec2 uvs = %s; vec4 b = textureLod(t0, uvs, 0.0);\n"
                "      float zz = min(abs(tw), 1.0) - uShadow.x;\n"
                "      float zref = b.a * 0.99609375 + b.b * 0.00389099 + b.g * 0.0000152;\n"
                "      tx0 = vec4(zref >= zz ? b.r : 0.0); }\n",
                t.shadow_ortho ? "c0" : "c0 / tw");
            continue;
        }
        const char *coord = t.coord == 0 ? "c0" : t.coord == 1 ? "c1" : "c2";
        g.f("    { vec2 uv = %s%s;\n", coord, type == 3 ? " / tw" : "");
        std::string border;
        auto bcond = [&](u32 wrap, const char *c) {
            if (wrap == 1) border += std::string(border.empty() ? "" : " || ") + "uv." + c + " < 0.0 || uv." + c + " >= 1.0";
            if (wrap == 5) border += std::string(border.empty() ? "" : " || ") + "uv." + c + " >= 1.0";
        };
        bcond(t.wrap_s, "x");
        bcond(t.wrap_t, "y");
        if (!border.empty())
            g.f("      tx%d = (%s) ? uBorder[%d] : texture(t%d, uv); }\n", i, border.c_str(), i, i);
        else
            g.f("      tx%d = texture(t%d, uv); }\n", i, i);
    }
    if (proctex) g.f("    if (uPt[2].w != 0.0) tx3 = pt_sample(c0 * uPt[2].x + c1 * uPt[2].y + c2 * uPt[2].z);\n");
    if (ds.lighting) gen_lighting(g, ds.light);
    g.f("    vec4 outc = uTevInit[0], buf = uTevInit[1], nxt = uTevInit[2];\n");
    for (u32 li = 0; li < ds.tev_count; li++) {
        const u32 si = ds.tev_list[li];
        const TevStageState &t = ds.tev[si];
        g.f("    { // stage %u\n      vec4 K = uKonst[%u];\n", si, si);
        for (int k = 0; k < 3; k++) {
            g.f("      vec3 cs%d = %s;\n", k, color_mod(t.cmod[k], src_expr(t.csrc[k])).c_str());
            g.f("      float as%d = %s;\n", k, alpha_mod(t.amod[k], src_expr(t.asrc[k])).c_str());
        }
        if (t.cop == 6 || t.cop == 7)
            g.f("      vec3 r = vec3(clamp(4.0 * dot(cs0 - 0.50196, cs1 - 0.50196), 0.0, 1.0));\n");
        else
            g.f("      vec3 r = %s;\n", combine(t.cop, "cs0", "cs1", "cs2", true).c_str());
        if (t.cop == 7)
            g.f("      float a = r.x;\n");
        else if (t.aop == 6 || t.aop == 7)
            g.f("      float a = 0.0;\n");
        else
            g.f("      float a = %s;\n", combine(t.aop, "as0", "as1", "as2", false).c_str());
        if (t.cmul != 1) g.f("      r = min(r * %u.0, vec3(1.0));\n", t.cmul);
        if (t.amul != 1) g.f("      a = min(a * %u.0, 1.0);\n", t.amul);
        g.f("      outc = vec4(r, a);\n      buf = nxt;\n");
        if (t.upd_rgb) g.f("      nxt.rgb = outc.rgb;\n");
        if (t.upd_a) g.f("      nxt.a = outc.a;\n");
        g.f("    }\n");
    }
    if (ds.alpha_test) g.f("    { float A = floor(outc.a * 255.0 + 0.5), B = uDepth.z; if (!(%s)) discard; }\n", cmp_expr(ds.alpha_func));
    if (ds.fog)
        g.f("    { float fi = (%s) * 128.0; float fl = clamp(floor(fi), 0.0, 127.0);\n"
            "      vec2 fe = lut_entry(texelFetch(uFog, ivec2(int(fl), 0), 0)); float ff = clamp(fe.x + fe.y * (fi - fl), 0.0, 1.0);\n"
            "      outc.rgb = ff * outc.rgb + (1.0 - ff) * uFogColor.rgb; }\n",
            ds.fog_flip ? "1.0 - zd" : "zd");
    g.f("    FragColor = outc;\n}\n");
    return g.s;
}

// For combiner settings no run has logged: reads them from uniforms and does what gen_fs_glsl() would have compiled in, far more slowly, but with the right colours. A miss draws
// with this until a build has its own shader. The plain fallbacks have no combiners, lighting or alpha test, so cut-outs draw solid and lit things go black. Leaves out cube maps,
// shadow maps and the procedural texture, and the lit one stops at 4 lights, which no logged state passes.
std::string gen_uber_fs(bool lit) {
    std::string s = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec4 vTc01;
layout(location = 2) in vec4 vTc2w;
layout(location = 5) in vec2 vZw;
)";
    if (lit)
        s += R"(layout(location = 3) in vec4 vQuat;
layout(location = 4) in vec4 vView;
layout(location = 6) in vec4 vTan;
)";
    s += R"(layout(location = 0) out vec4 FragColor;
struct Light { vec4 spec0, spec1, diffuse, ambient, pos, spot, att; };
layout(binding = 0, std140) uniform Uni {
    vec4 uDepth;
    ivec4 uScissor;
    vec4 uKonst[6];
    vec4 uTevInit[3];
    vec4 uFogColor;
    vec4 uBorder[3];
    vec4 uShadow;
    vec4 uLscale0, uLscale1, uAmbient;
    Light uLights[8];
    ivec4 uCsrc[6];     // colour sources, then the operation
    ivec4 uCmod[6];     // their modifiers, then the scale
    ivec4 uAsrc[6];     // the same for alpha
    ivec4 uAmod[6];
    ivec4 uStage[6];    // stage number, update rgb, update alpha
    vec4 uTexCoord[3];  // how much of each coordinate set a unit reads, then how much of its texture to use
    vec4 uTexAdd[3];    // what a null texture reads as
    ivec4 uWrap[3];
    ivec4 uUber;        // stages, alpha test function or -1, fog (2: flipped), unit 0 is projected
)";
    if (lit)
        s += R"(    ivec4 uLit;         // lights, bump mode, bump unit, renormalize the bump
    ivec4 uLit2;        // the tangent term is in use, shadow unit or -1, invert the shadow, shadow on alpha
    ivec4 uLit3;        // primary alpha, secondary alpha, clamp highlights
    ivec4 uLit4;        // shadow on primary, on secondary
    ivec4 uSel[7];      // d0 d1 sp fr rb rg rr: on, abs, input
    ivec4 uLight[4];    // LUT number, directional, two sided, shadow
    ivec4 uLight2[4];   // geometry factor 0, 1, distance attenuation, spot attenuation
)";
    s += R"(};
layout(binding = 0) uniform sampler2D t0;
layout(binding = 1) uniform sampler2D t1;
layout(binding = 2) uniform sampler2D t2;
layout(binding = 9) uniform sampler2D uFog;
vec4 prim, fpc, fsc, tx0, tx1, tx2, outc, buf, nxt, K;
vec2 lut_entry(vec4 t) { return vec2(t.r * 0.99609375 + t.g * 0.00389105, (t.b * 0.99609375 + t.a * 0.00389105) * 2.0 - 1.0); }
vec4 source(int s) {
    if (s == 0) return prim;
    if (s == 1) return fpc;
    if (s == 2) return fsc;
    if (s == 3) return tx0;
    if (s == 4) return tx1;
    if (s == 5) return tx2;
    if (s == 13) return buf;
    if (s == 14) return K;
    if (s == 15) return outc;
    return vec4(0.0);
}
vec3 colour_operand(int s, int m) {
    vec4 v = source(s);
    vec3 r = v.rgb;
    int which = m & 14;
    if (which == 2) r = v.aaa;
    else if (which == 4) r = v.rrr;
    else if (which == 8) r = v.ggg;
    else if (which == 12) r = v.bbb;
    return (m & 1) != 0 ? 1.0 - r : r;
}
float alpha_operand(int s, int m) {
    vec4 v = source(s);
    int which = (m >> 1) & 3;
    float r = v.a;
    if (which == 1) r = v.r;
    else if (which == 2) r = v.g;
    else if (which == 3) r = v.b;
    return (m & 1) != 0 ? 1.0 - r : r;
}
vec3 combine_colour(int op, vec3 a, vec3 b, vec3 c) {
    if (op == 1) return a * b;
    if (op == 2) return min(a + b, vec3(1.0));
    if (op == 3) return clamp(a + b - 0.50196, 0.0, 1.0);
    if (op == 4) return a * c + b * (1.0 - c);
    if (op == 5) return max(a - b, vec3(0.0));
    if (op == 6 || op == 7) return vec3(clamp(4.0 * dot(a - 0.50196, b - 0.50196), 0.0, 1.0));
    if (op == 8) return min(a * b + c, vec3(1.0));
    if (op == 9) return min(a + b, vec3(1.0)) * c;
    return a;
}
float combine_alpha(int op, float a, float b, float c) {
    if (op == 1) return a * b;
    if (op == 2) return min(a + b, 1.0);
    if (op == 3) return clamp(a + b - 0.50196, 0.0, 1.0);
    if (op == 4) return a * c + b * (1.0 - c);
    if (op == 5) return max(a - b, 0.0);
    if (op == 6 || op == 7) return 0.0;
    if (op == 8) return min(a * b + c, 1.0);
    if (op == 9) return min(a + b, 1.0) * c;
    return a;
}
void stage(int i) {
    K = uKonst[uStage[i].x];
    vec3 r = combine_colour(uCsrc[i].w, colour_operand(uCsrc[i].x, uCmod[i].x), colour_operand(uCsrc[i].y, uCmod[i].y), colour_operand(uCsrc[i].z, uCmod[i].z));
    float a = uCsrc[i].w == 7 ? r.x : combine_alpha(uAsrc[i].w, alpha_operand(uAsrc[i].x, uAmod[i].x), alpha_operand(uAsrc[i].y, uAmod[i].y), alpha_operand(uAsrc[i].z, uAmod[i].z));
    if (uCmod[i].w != 1) r = min(r * float(uCmod[i].w), vec3(1.0));
    if (uAmod[i].w != 1) a = min(a * float(uAmod[i].w), 1.0);
    outc = vec4(r, a);
    buf = nxt;
    if (uStage[i].y != 0) nxt.rgb = outc.rgb;
    if (uStage[i].z != 0) nxt.a = outc.a;
}
vec4 sampled(int i, vec2 uv, vec4 t) {
    bool border = (uWrap[i].x == 1 && (uv.x < 0.0 || uv.x >= 1.0)) || (uWrap[i].x == 5 && uv.x >= 1.0) ||
                  (uWrap[i].y == 1 && (uv.y < 0.0 || uv.y >= 1.0)) || (uWrap[i].y == 5 && uv.y >= 1.0);
    return border ? uBorder[i] : t * uTexCoord[i].w + uTexAdd[i];
}
)";
    // gen_lighting(), with what it compiles in read from uniforms
    if (lit)
        s += R"(layout(binding = 8) uniform sampler2D uLut;
vec3 normal, tangent, view, nv;
vec4 dsum, ssum, shadow;
float lin0, lin1, lin2, lin3, lin4, lin5;
vec4 unit_output(int i) {
    if (i == 0) return tx0;
    if (i == 1) return tx1;
    if (i == 2) return tx2;
    return vec4(0.0);
}
float lutv(int s, float r, bool absm, bool two) {
    int idx; float delta;
    if (absm) { r = two ? abs(r) : max(r, 0.0); float fl = floor(r * 256.0); idx = int(clamp(fl, 0.0, 255.0)); delta = r * 256.0 - float(idx); }
    else { float fl = floor(r * 128.0); int si = int(clamp(fl, -128.0, 127.0)); delta = r * 128.0 - float(si); idx = si < 0 ? si + 256 : si; }
    vec2 e = lut_entry(texelFetch(uLut, ivec2(idx, s), 0));
    return e.x + e.y * delta;
}
float lut_input(int i) {
    if (i == 0) return lin0;
    if (i == 1) return lin1;
    if (i == 2) return lin2;
    if (i == 3) return lin3;
    if (i == 4) return lin4;
    if (i == 5) return lin5;
    return 0.0;
}
float lut(int sel, int row, bool two) { return lutv(row, lut_input(uSel[sel].z), uSel[sel].y != 0, two); }
void light(int i) {
    ivec4 f = uLight[i], h = uLight2[i];
    vec3 lv = uLights[i].pos.xyz;
    if (f.y == 0) lv += view;
    float len = length(lv);
    if (len > 0.0) lv = lv * (1.0 / len);
    vec3 hv = nv + lv;
    float dist = 1.0;
    if (h.z != 0) {
        float loc = clamp(uLights[i].att.x * len + uLights[i].att.y, 0.0, 1.0), fl = floor(loc * 256.0);
        int idx = int(clamp(fl, 0.0, 255.0));
        vec2 e = lut_entry(texelFetch(uLut, ivec2(idx, 16 + f.x), 0));
        dist = e.x + e.y * (loc * 256.0 - float(idx));
    }
    float hl = length(hv);
    vec3 nh = hl > 0.0 ? hv * (1.0 / hl) : hv;
    lin0 = dot(normal, nh); lin1 = dot(nv, nh); lin2 = dot(normal, nv); lin3 = dot(lv, normal); lin4 = dot(lv, uLights[i].spot.xyz);
    lin5 = uLit2.x != 0 ? dot(nh - normal * lin0, tangent) : 0.0;
    bool two = f.z != 0;
    float spot = 1.0, d0 = 1.0, d1 = 1.0;
    if (h.w != 0 && uSel[2].x != 0) spot = uLscale0.z * lut(2, 8 + f.x, two);
    if (uSel[0].x != 0) d0 = uLscale0.x * lut(0, 0, two);
    vec3 refl = vec3(1.0);
    if (uSel[6].x != 0) refl.x = uLscale1.z * lut(6, 6, two);
    refl.y = refl.z = refl.x;
    if (uSel[5].x != 0) refl.y = uLscale1.y * lut(5, 5, two);
    if (uSel[4].x != 0) refl.z = uLscale1.x * lut(4, 4, two);
    if (uSel[1].x != 0) d1 = uLscale0.y * lut(1, 1, two);
    if (i == uLit.x - 1 && uSel[3].x != 0) {
        float fr = uLscale0.w * lut(3, 3, two);
        if (uLit3.x != 0) dsum.a = fr;
        if (uLit3.y != 0) ssum.a = fr;
    }
    float dp = two ? abs(lin3) : max(lin3, 0.0);
    float clamp_h = uLit3.z != 0 && dp == 0.0 ? 0.0 : 1.0;
    float g0 = 1.0, g1 = 1.0;
    if (h.x != 0 || h.y != 0) {
        float h2 = dot(hv, hv), geo = h2 == 0.0 ? 0.0 : min(dp / h2, 1.0);
        if (h.x != 0) g0 = geo;
        if (h.y != 0) g1 = geo;
    }
    float att = dist * spot;
    vec3 diffuse = uLights[i].diffuse.rgb * dp, specular = (d0 * uLights[i].spec0.rgb * g0 + d1 * refl * uLights[i].spec1.rgb * g1) * clamp_h * att;
    if (f.w != 0 && uLit4.x != 0) diffuse *= shadow.rgb;
    if (f.w != 0 && uLit4.y != 0) specular *= shadow.rgb;
    dsum.rgb += (diffuse + uLights[i].ambient.rgb) * att;
    ssum.rgb += specular;
}
void lighting() {
    shadow = vec4(1.0);
    if (uLit2.y >= 0) {
        shadow = unit_output(uLit2.y);
        if (uLit2.z != 0) shadow = 1.0 - shadow;
    }
    vec3 sn = vec3(0.0, 0.0, 1.0), st = vec3(1.0, 0.0, 0.0);
    bool bump = uLit.y == 1 || uLit.y == 2;
    if (bump) {
        vec3 p = unit_output(uLit.z).rgb * 2.00787 - 1.0;
        if (uLit.y == 1) {
            if (uLit.w != 0) p.z = sqrt(max(1.0 - (p.x * p.x + p.y * p.y), 0.0));
            sn = p;
        } else {
            st = p;
        }
    }
    vec3 ln = vQuat.xyz;
    float d = length(ln);
    ln = d > 0.0 ? ln * (1.0 / d) : vec3(0.0, 0.0, 1.0);
    normal = ln;
    tangent = vec3(1.0, 0.0, 0.0);
    if (bump || uLit2.x != 0) {
        vec3 lt = vTan.xyz;
        d = length(lt);
        lt = d > 0.0 ? lt * (1.0 / d) : vec3(1.0, 0.0, 0.0);
        vec3 lb = cross(ln, lt);
        normal = lt * sn.x + lb * sn.y + ln * sn.z;
        tangent = lt * st.x + lb * st.y + ln * st.z;
    }
    view = vView.xyz;
    float vl = length(view);
    nv = vl > 0.0 ? view * (1.0 / vl) : view;
    dsum = vec4(0.0, 0.0, 0.0, 1.0);
    ssum = vec4(0.0, 0.0, 0.0, 1.0);
    if (uLit.x > 0) light(0);
    if (uLit.x > 1) light(1);
    if (uLit.x > 2) light(2);
    if (uLit.x > 3) light(3);
    if (uLit2.w != 0) {
        if (uLit3.x != 0) dsum.a *= shadow.a;
        if (uLit3.y != 0) ssum.a *= shadow.a;
    }
    dsum.rgb += uAmbient.rgb;
    fpc = clamp(dsum, 0.0, 1.0);
    fsc = clamp(ssum, 0.0, 1.0);
}
)";
    s += R"(void main() {
    prim = clamp(vColor, 0.0, 1.0);
    fpc = fsc = vec4(0.0);
    vec2 c0 = vTc01.xy, c1 = vTc01.zw, c2 = vTc2w.xy;
    vec2 u0 = c0 * uTexCoord[0].x + c1 * uTexCoord[0].y + c2 * uTexCoord[0].z;
    if (uUber.w != 0) u0 = u0 * (1.0 / vTc2w.z);
    vec2 u1 = c0 * uTexCoord[1].x + c1 * uTexCoord[1].y + c2 * uTexCoord[1].z;
    vec2 u2 = c0 * uTexCoord[2].x + c1 * uTexCoord[2].y + c2 * uTexCoord[2].z;
    tx0 = sampled(0, u0, texture(t0, u0));
    tx1 = sampled(1, u1, texture(t1, u1));
    tx2 = sampled(2, u2, texture(t2, u2));
)";
    if (lit) s += "    lighting();\n";
    s += R"(    outc = uTevInit[0]; buf = uTevInit[1]; nxt = uTevInit[2];
    if (uUber.x > 0) stage(0);
    if (uUber.x > 1) stage(1);
    if (uUber.x > 2) stage(2);
    if (uUber.x > 3) stage(3);
    if (uUber.x > 4) stage(4);
    if (uUber.x > 5) stage(5);
    if (uUber.y >= 0) {
        float A = floor(outc.a * 255.0 + 0.5), B = uDepth.z;
        int f = uUber.y;
        bool pass = f == 1 || (f == 2 && A == B) || (f == 3 && A != B) || (f == 4 && A < B) || (f == 5 && A <= B) || (f == 6 && A > B) || (f == 7 && A >= B);
        if (!pass) discard;
    }
    if (uUber.z != 0) {
        float zd = clamp(vZw.x / vZw.y * uDepth.x + uDepth.y, 0.0, 1.0);
        float fi = (uUber.z == 2 ? 1.0 - zd : zd) * 128.0, fl = clamp(floor(fi), 0.0, 127.0);
        vec2 fe = lut_entry(texelFetch(uFog, ivec2(int(fl), 0), 0));
        float ff = clamp(fe.x + fe.y * (fi - fl), 0.0, 1.0);
        outc.rgb = ff * outc.rgb + (1.0 - ff) * uFogColor.rgb;
    }
    FragColor = outc;
}
)";
    return s;
}

// Everything but the program's body, which shader_glsl_spec() writes. Output registers are mapped to what the rasterizer needs
// here, so a program used with two output maps is two vertex shaders. Y is negated: rows in a surface are PICA window rows, y up.
std::string gen_vs_glsl(const ShaderProgram *p, u32 entry, u32 b, const u8 (*iu)[4], const s8 *slot_src, u32 *inputs) {
    u32 used = 0;
    const std::string body = shader_glsl_spec(p, entry, b, iu, &used);
    if (body.empty()) return "";
    *inputs = used;
    const bool outline = slot_src[0] == kOutlineSlot;
    Gen g;
    g.s = "#version 450\n";
    for (u32 r = 0; r < 16; r++)
        if (used & (1u << r)) g.f("layout(location = %u) in vec4 v%u;\n", r, r);
    g.f("layout(location = 0) out vec4 vColor;\n"
        "layout(location = 1) out vec4 vTc01;\n"
        "layout(location = 2) out vec4 vTc2w;\n"
        "layout(location = 3) out vec4 vQuat;\n"
        "layout(location = 4) out vec4 vView;\n"
        "layout(location = 5) out vec2 vZw;\n"
        "layout(location = 6) out vec4 vTan;\n"
        "layout(binding = 0, std140) uniform VS { vec4 f[96]; vec4 uDraw; ivec4 uInt[4];%s };\n"  // uInt: loop counts, starts and steps
        "vec4 o[16];\n"
        "vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0), t4 = vec4(0.0), t5 = vec4(0.0), t6 = vec4(0.0), t7 = vec4(0.0);\n"
        "vec4 t8 = vec4(0.0), t9 = vec4(0.0), t10 = vec4(0.0), t11 = vec4(0.0), t12 = vec4(0.0), t13 = vec4(0.0), t14 = vec4(0.0), t15 = vec4(0.0);\n"
        "int a0 = 0, a1 = 0, aL = 0; bool c0 = false, c1 = false;\n"
        "vec4 sel_ge(vec4 a, vec4 b) { return vec4(greaterThanEqual(a, b)); }\n"
        "vec4 sel_lt(vec4 a, vec4 b) { return vec4(lessThan(a, b)); }\n"
        "vec4 vs_mul(vec4 a, vec4 b) { return a * b; }\n"
        "vec4 vs_dp3(vec4 a, vec4 b) { return vec4(dot(a.xyz, b.xyz)); }\n"
        "vec4 vs_dp4(vec4 a, vec4 b) { return vec4(dot(a, b)); }\n"
        "vec4 vs_dph(vec4 a, vec4 b) { return vec4(dot(vec4(a.xyz, 1.0), b)); }\n"
        "vec4 vs_dst(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n"
        "vec4 vs_max(vec4 a, vec4 b) { return max(a, b); }\n"
        "vec4 vs_min(vec4 a, vec4 b) { return min(a, b); }\n"
        // On a PICA 0 * infinity is 0. Superstar Saga's menu program divides by a z of 0 and multiplies that by 0: NaN here put its text boxes nowhere. Every multiply
        // doing what a PICA does took up to 54 registers, over a real GPU's 48 (tools/shadergen_main.cpp checks), so reciprocals stop short of infinity instead: 1e20 is past any PICA float.
        "vec4 vs_lim(float x) { return vec4(clamp(x, -1e20, 1e20)); }\n"
        "int vs_s32(float x) { return int(x); }\n"
        "vec4 vs_rel(int base, int off) { if (off < -128 || off > 127) off = 0; int u = (base + off) & 127; return u >= 96 ? vec4(1.0) : f[u]; }\n",
        outline ? " vec4 uOutline[2];" : "");
    g.f("void vs_output() {\n");
    if (outline) {
        // what Level-5's outline geometry shader does to each vertex of a silhouette edge, for the whole mesh: output 2 is the direction.
        // Only near edge-on, so a crease facing the viewer doesn't push over the facet next to it and show as a line.
        const int normal = shader_outline_normal(p, entry);
        char edge[64] = "1.0", facing[64] = "0.0";
        if (normal >= 0) {
            snprintf(edge, sizeof edge, "clamp((0.6 - t%d.z) * 4.0, 0.0, 1.0)", normal);
            // The push also carries a vertex past where its neighbours' surface has already curved away, so over a steep facet it would sit in front of the model and show as a
            // line down a character's cheek. Back past anything of its own, what faces the viewer can't, and only the part round the silhouette is left.
            snprintf(facing, sizeof facing, "clamp((t%d.z - 0.1) * 3.0, 0.0, 1.0)", normal);
        }
        g.f("    vec4 p = o[0];\n"
            "    p.xy += uOutline[0].xy * o[2].xy * %s * (uOutline[0].z != 0.0 ? p.w : 1.0);\n"
            "    gl_Position = vec4(p.x, -p.y, p.z * uDraw.x + uDraw.y * p.w + uDraw.z * %s * p.w, p.w);\n"
            "    vZw = p.zw;\n"
            "    vQuat = vec4(0.0, 0.0, 1.0, 0.0);\n"
            "    vTan = vec4(1.0, 0.0, 0.0, 0.0);\n"
            "    vColor = uOutline[1];\n"
            "    vTc01 = vec4(0.0);\n"
            "    vTc2w = vec4(0.0);\n"
            "    vView = vec4(0.0);\n"
            "}\nvoid main() {\n", edge, facing);
        g.s += body;
        g.f("    vs_output();\n}\n");
        return g.s;
    }
    auto slot = [&](u32 i) {
        const s32 s = slot_src[i];
        char x[32];
        if (s < 0) return std::string("1.0");
        snprintf(x, sizeof x, "o[%d].%c", s >> 2, "xyzw"[s & 3]);
        return std::string(x);
    };
    auto vec = [&](std::initializer_list<u32> slots, bool colour = false) {
        std::string r;
        for (u32 i : slots) {
            if (!r.empty()) r += ", ";
            r += colour ? "min(abs(" + slot(i) + "), 1.0)" : slot(i);
        }
        return r;
    };
    g.f("    vec4 p = vec4(%s);\n", vec({0, 1, 2, 3}).c_str());
    g.f("    gl_Position = vec4(p.x, -p.y, p.z * uDraw.x + uDraw.y * p.w, p.w);\n");
    g.f("    vZw = p.zw;\n");
    g.f("    vec4 q = vec4(%s);\n", vec({4, 5, 6, 7}).c_str());
    g.s += frame_outputs;
    g.f("    vColor = vec4(%s);\n", vec({8, 9, 10, 11}, true).c_str());
    g.f("    vTc01 = vec4(%s);\n", vec({12, 13, 14, 15}).c_str());
    g.f("    vTc2w = vec4(%s, %s, 0.0);\n", vec({22, 23}).c_str(), slot(16).c_str());
    g.f("    vView = vec4(%s, 0.0);\n", vec({18, 19, 20}).c_str());
    g.f("}\nvoid main() {\n");
    g.s += body;
    g.f("    vs_output();\n}\n");
    return g.s;
}

std::string gen_passthrough_vs() {
    return "#version 450\n"
           "layout(location = 0) in vec4 aPos;\n"
           "layout(location = 1) in vec4 aQuat;\n"
           "layout(location = 2) in vec4 aColor;\n"
           "layout(location = 3) in vec4 aTc01;\n"
           "layout(location = 4) in vec4 aTc2w;\n"
           "layout(location = 5) in vec4 aView;\n"
           "layout(location = 6) in vec4 aTc0w;\n"
           "layout(location = 0) out vec4 vColor;\n"
           "layout(location = 1) out vec4 vTc01;\n"
           "layout(location = 2) out vec4 vTc2w;\n"
           "layout(location = 3) out vec4 vQuat;\n"
           "layout(location = 4) out vec4 vView;\n"
           "layout(location = 5) out vec2 vZw;\n"
           "layout(location = 6) out vec4 vTan;\n"
           "layout(binding = 0, std140) uniform VS { vec4 f[96]; vec4 uDraw; };\n"
           "void main() {\n"
           "    gl_Position = vec4(aPos.x, -aPos.y, aPos.z * uDraw.x + uDraw.y * aPos.w, aPos.w);\n"
           "    vZw = aPos.zw; vColor = aColor; vTc01 = aTc01; vTc2w = vec4(aTc2w.xy, aTc0w.x, 0.0); vView = aView;\n"
           "    vec4 q = aQuat;\n" +
           std::string(frame_outputs) + "}\n";
}

// the screens drawn onto the TV and GamePad: a textured quad, rotated by its texture coordinates
std::string gen_blit_vs() {
    return "#version 450\n"
           "layout(location = 0) in vec4 aPosUv;\n"
           "layout(location = 0) out vec2 vUv;\n"
           "void main() { gl_Position = vec4(aPosUv.xy, 0.0, 1.0); vUv = aPosUv.zw; }\n";
}
std::string gen_blit_fs() {
    return "#version 450\n"
           "layout(location = 0) in vec2 vUv;\n"
           "layout(location = 0) out vec4 FragColor;\n"
           "layout(binding = 0) uniform sampler2D tex;\n"
           "layout(binding = 0, std140) uniform Blit { vec4 uTint; vec4 uAdd; };\n"
           "void main() { FragColor = texture(tex, vUv) * uTint + uAdd; }\n";
}

// An ETC1 or ETC1A4 texture decoded by the GPU: the texture's bytes, copied as they are in the game's memory into an RGBA8 staging texture
// (bytes, so no byte order question, as the tables) of one block per 2 or 4 texels, block i at texel (i % blocks per row) * k, in the PICA's
// order: 8x8 tiles row by row, four 4x4 blocks per tile, alpha nibbles before the colour word in ETC1A4. vUv is the pixel position on the
// target, rows from its first; the PICA's first memory row is the texture's top, so it lands on the last row, as the CPU decoder lays it out.
// The decoding is decode_etc1_block's, texel by texel.
std::string gen_etc1_fs(bool alpha) {
    // no table with a computed index and no size query: Cemu's translator refuses both; the sizes come in the blit uniform block (uTint)
    std::string s = "#version 450\n"
                    "layout(location = 0) in vec2 vUv;\n"
                    "layout(location = 0) out vec4 FragColor;\n"
                    "layout(binding = 0) uniform sampler2D tBlocks;\n"
                    "layout(binding = 0, std140) uniform Blit { vec4 uTint; vec4 uAdd; };\n"
                    "uint word(int bx, int by) { uvec4 b = uvec4(round(texelFetch(tBlocks, ivec2(bx, by), 0) * 255.0)); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
                    "int c4(uint v) { return int(v) * 17; }\n"
                    "int c5(uint v) { return int((v << 3) | (v >> 2)); }\n"
                    "int s3(uint v) { return int(v & 7u) - (((v & 4u) != 0u) ? 8 : 0); }\n"
                    "int small(int t) { return t < 4 ? (t < 2 ? (t == 0 ? 2 : 5) : (t == 2 ? 9 : 13)) : (t < 6 ? (t == 4 ? 18 : 24) : (t == 6 ? 33 : 47)); }\n"
                    "int big(int t) { return t < 4 ? (t < 2 ? (t == 0 ? 8 : 17) : (t == 2 ? 29 : 42)) : (t < 6 ? (t == 4 ? 60 : 80) : (t == 6 ? 106 : 183)); }\n"
                    "void main() {\n";
    s += alpha ? "    const int k = 4;\n" : "    const int k = 2;\n";
    // two staging rows to a row of tiles, so no division, which compiles to an instruction Cemu's translator doesn't have
    s += "    int w = int(uTint.x), h = int(uTint.y), bw = w >> 2;\n"
         "    int x = int(vUv.x), m = h - 1 - int(vUv.y);\n"
         "    int n = ((x >> 3) << 2) + ((x >> 2) & 1) + (((m >> 2) & 1) << 1);\n"
         "    int by = (m >> 3) << 1, bx = n;\n"
         "    if (n >= bw) { bx = n - bw; by += 1; }\n"
         "    bx *= k;\n";
    s += alpha ? "    uint alo = word(bx, by), ahi = word(bx + 1, by), lo = word(bx + 2, by), hi = word(bx + 3, by);\n"
               : "    uint lo = word(bx, by), hi = word(bx + 1, by);\n";
    s += "    bool flip = (hi & 1u) != 0u, diff = (hi & 2u) != 0u;\n"
         "    ivec3 base0, base1;\n"
         "    if (diff) {\n"
         "        uint r = (hi >> 27) & 31u, g = (hi >> 19) & 31u, b = (hi >> 11) & 31u;\n"
         "        base0 = ivec3(c5(r), c5(g), c5(b));\n"
         "        base1 = ivec3(c5(uint(int(r) + s3(hi >> 24)) & 31u), c5(uint(int(g) + s3(hi >> 16)) & 31u), c5(uint(int(b) + s3(hi >> 8)) & 31u));\n"
         "    } else {\n"
         "        base0 = ivec3(c4((hi >> 28) & 15u), c4((hi >> 20) & 15u), c4((hi >> 12) & 15u));\n"
         "        base1 = ivec3(c4((hi >> 24) & 15u), c4((hi >> 16) & 15u), c4((hi >> 8) & 15u));\n"
         "    }\n"
         "    int tx = x & 3, ty = m & 3, texel = 4 * tx + ty;\n"
         "    bool second = flip ? ty >= 2 : tx >= 2;\n"
         "    int table = int(second ? (hi >> 2) & 7u : (hi >> 5) & 7u);\n"
         "    int mod = ((lo >> texel) & 1u) != 0u ? big(table) : small(table);\n"
         "    if (((lo >> (16 + texel)) & 1u) != 0u) mod = -mod;\n"
         "    ivec3 c = clamp((second ? base1 : base0) + mod, 0, 255);\n";
    s += alpha ? "    int a = c4((texel < 8 ? alo >> (4 * texel) : ahi >> (4 * (texel - 8))) & 15u);\n" : "    int a = 255;\n";
    s += "    FragColor = vec4(c, a) / 255.0;\n"
         "}\n";
    return s;
}

// A texture of one of the PICA's plain formats (0 to 11) decoded by the GPU, as gen_etc1_fs does those two: the bytes as they are in the game's
// memory in an RGBA8 staging texture, 8 staging rows to a row of 8x8 tiles (so a row of tiles starts a staging row, whatever the format's
// size), and each pixel finds its texel through the tile's Morton order. uTint: the texture's width and height, 1 / the staging width and the
// staging width. The colours are decode_level's.
std::string gen_untile_fs(u32 fmt) {
    static const char *const colour[12] = {
        "    ivec4 c = ivec4(px(i, row)).wzyx;\n",
        "    ivec4 c = ivec4(byte_at(3 * i + 2, row), byte_at(3 * i + 1, row), byte_at(3 * i, row), 255);\n",
        "    int v = pair(i, row); ivec4 c = ivec4(c5(v >> 11), c5((v >> 6) & 31), c5((v >> 1) & 31), (v & 1) * 255);\n",
        "    int v = pair(i, row); ivec4 c = ivec4(c5(v >> 11), c6((v >> 5) & 63), c5(v & 31), 255);\n",
        "    int v = pair(i, row); ivec4 c = ivec4(c4(v >> 12), c4((v >> 8) & 15), c4((v >> 4) & 15), c4(v & 15));\n",
        "    int v = pair(i, row); ivec4 c = ivec4(ivec3(v >> 8), v & 255);\n",
        "    int v = pair(i, row); ivec4 c = ivec4(v >> 8, v & 255, 0, 255);\n",
        "    ivec4 c = ivec4(ivec3(byte_at(i, row)), 255);\n",
        "    ivec4 c = ivec4(0, 0, 0, byte_at(i, row));\n",
        "    int v = byte_at(i, row); ivec4 c = ivec4(ivec3(c4(v >> 4)), c4(v & 15));\n",
        "    int v = byte_at(i >> 1, row); ivec4 c = ivec4(ivec3(c4((i & 1) != 0 ? v >> 4 : v & 15)), 255);\n",
        "    int v = byte_at(i >> 1, row); ivec4 c = ivec4(0, 0, 0, c4((i & 1) != 0 ? v >> 4 : v & 15));\n",
    };
    // the staging row by a multiplication: integer division compiles to an instruction Cemu's translator doesn't have (gen_etc1_fs)
    return std::string("#version 450\n"
                       "layout(location = 0) in vec2 vUv;\n"
                       "layout(location = 0) out vec4 FragColor;\n"
                       "layout(binding = 0) uniform sampler2D tBytes;\n"
                       "layout(binding = 0, std140) uniform Blit { vec4 uTint; vec4 uAdd; };\n"
                       "vec4 px(int q, int row) { int r = int((float(q) + 0.5) * uTint.z); return round(texelFetch(tBytes, ivec2(q - r * int(uTint.w), row + r), 0) * 255.0); }\n"
                       "int byte_at(int k, int row) { vec4 p = px(k >> 2, row); int n = k & 3; return int(n < 2 ? (n == 0 ? p.x : p.y) : (n == 2 ? p.z : p.w)); }\n"
                       "int pair(int i, int row) { vec4 p = px(i >> 1, row); return (i & 1) == 0 ? int(p.x) | (int(p.y) << 8) : int(p.z) | (int(p.w) << 8); }\n"
                       "int c4(int v) { return v * 17; }\n"
                       "int c5(int v) { return (v << 3) | (v >> 2); }\n"
                       "int c6(int v) { return (v << 2) | (v >> 4); }\n"
                       "void main() {\n"
                       "    int x = int(vUv.x), m = int(uTint.y) - 1 - int(vUv.y), xi = x & 7, yi = m & 7;\n"
                       "    int i = ((x >> 3) << 6) + ((xi & 1) | ((yi & 1) << 1) | ((xi & 2) << 1) | ((yi & 2) << 2) | ((xi & 4) << 2) | ((yi & 4) << 3));\n"
                       "    int row = (m >> 3) << 3;\n") +
           colour[fmt] + "    FragColor = vec4(c) / 255.0;\n}\n";
}
std::string untile_key(u32 fmt) { return "untile" + std::to_string(fmt); }
}  // namespace gpu
