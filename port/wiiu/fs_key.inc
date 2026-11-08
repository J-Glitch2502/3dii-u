// From runtime/gpu/metal.mm (nathsou/3dsrecomp, EUPL-1.2): the keys in games/<title>/pipelines.bin use this layout.
// can the PICA blend / logic op be done by Metal's fixed-function blender?
bool hw_blend(const DrawState &ds) {
    if (!ds.blend) return ds.logic_op == 3;  // copy
    auto ok = [](u32 eq, u32 sf, u32 df) { return eq <= 2 || (eq <= 4 && sf == 1 && df == 1); };
    return ok(ds.eq_rgb, ds.sf_rgb, ds.df_rgb) && ok(ds.eq_a, ds.sf_a, ds.df_a);
}
// depth computed per fragment only for W-buffering (Z-buffer depth is affine in screen space)
bool frag_depth(const DrawState &ds) { return ds.wbuffer; }

// key describing everything the generated fragment shader depends on
u32 fs_key_words(const DrawState &ds, u32 *w) {
    u32 n = 0;
    auto put = [&](u32 v) { w[n++] = le32(v); };
    put(ds.need_shade | ds.wbuffer << 1 | (ds.scissor_mode == 1) << 2 | ds.depth_bits << 3 | ds.color_write << 9 |
        hw_blend(ds) << 10);
    if (!ds.need_shade) return n;
    for (int i = 0; i < 3; i++) {
        const TexUnitState &t = ds.tex[i];
        put(ds.need_tex[i] | t.null_tex << 1 | t.type << 2 | t.coord << 6 | t.wrap_s << 8 | t.wrap_t << 11 |
            t.shadow_ortho << 14);
    }
    put(ds.tev_count);
    for (u32 i = 0; i < ds.tev_count; i++) {
        const u32 si = ds.tev_list[i];
        const TevStageState &t = ds.tev[si];
        put(si);
        for (int j = 0; j < 3; j++) put(t.csrc[j] | t.asrc[j] << 4 | t.cmod[j] << 8 | t.amod[j] << 12);
        put(t.cop | t.aop << 4 | t.cmul << 8 | t.amul << 12 | t.upd_rgb << 16 | t.upd_a << 17);
    }
    put(ds.lighting);
    if (ds.lighting) {
        const LightingState &l = ds.light;
        put(l.num_lights | l.config << 4 | l.bump_mode << 8 | l.bump_sel << 10 | l.shadow_sel << 12 |
            l.bump_renorm << 14 | l.shadow_enable << 15 | l.shadow_invert << 16 | l.shadow_primary << 17 |
            l.shadow_secondary << 18 | l.shadow_alpha << 19 | l.primary_alpha << 20 | l.secondary_alpha << 21 |
            l.clamp_highlights << 22);
        const LutSel *sels[7] = {&l.d0, &l.d1, &l.sp, &l.fr, &l.rb, &l.rg, &l.rr};
        for (auto *s : sels) put(s->enabled | s->abs << 1 | s->input << 2);
        for (u32 i = 0; i < l.num_lights; i++) {
            const LightState &lt = l.lights[i];
            put(lt.num | lt.directional << 3 | lt.two_sided << 4 | lt.geo0 << 5 | lt.geo1 << 6 | lt.dist_atten << 7 |
                lt.spot_atten << 8 | lt.shadow << 9);
        }
    }
    put(ds.fog | ds.fog_flip << 1 | ds.alpha_test << 2 | ds.alpha_func << 3);
    put(ds.blend | ds.eq_rgb << 1 | ds.eq_a << 4 | ds.sf_rgb << 8 | ds.df_rgb << 12 | ds.sf_a << 16 |
        ds.df_a << 20 | ds.logic_op << 24);
    put(ds.color_mask);
    return n;
}

std::string fs_key(const DrawState &ds) {
    u32 w[FS_KEY_WORDS];
    const u32 n = fs_key_words(ds, w);
    return std::string((const char *)w, n * 4);
}

// The same state without what the GX2 shader never reads: blending and masks (the hardware does those), wrap modes that need no
// border colour, operands a combiner's operation ignores. Shaders compile from this, so a state seen only on the console, differing
// from a logged one in just these, still finds its shader.
DrawState fs_normal(const DrawState &in) {
    DrawState ds = in;
    ds.depth_bits = 0;
    ds.color_write = false;
    ds.blend = false;
    ds.eq_rgb = ds.eq_a = ds.sf_rgb = ds.df_rgb = ds.sf_a = ds.df_a = 0;
    ds.logic_op = 3;  // so hw_blend() is always true
    ds.color_mask = 0;
    if (!ds.alpha_test) ds.alpha_func = 0;
    if (!ds.fog) ds.fog_flip = false;
    if (ds.scissor_mode != 1) ds.scissor_mode = 0;
    if (!ds.need_shade) return ds;
    for (int i = 0; i < 3; i++) {
        TexUnitState &t = ds.tex[i];
        if (!ds.need_tex[i] || t.null_tex) {
            t.type = t.coord = t.wrap_s = t.wrap_t = t.shadow_ortho = 0;
            continue;
        }
        if (i != 0) t.type = 0;
        if (t.type != 2) t.shadow_ortho = 0;
        if (t.wrap_s != 1 && t.wrap_s != 5) t.wrap_s = 0;
        if (t.wrap_t != 1 && t.wrap_t != 5) t.wrap_t = 0;
        if (t.type == 1 || t.type == 2 || t.type == 4) t.coord = t.wrap_s = t.wrap_t = 0;
    }
    auto operands = [](u32 op) { return op == 4 || op == 8 || op == 9 ? 3 : op == 1 || op == 2 || op == 3 || op == 5 || op == 6 || op == 7 ? 2 : 1; };
    for (u32 i = 0; i < ds.tev_count; i++) {
        TevStageState &t = ds.tev[ds.tev_list[i]];
        const int nc = operands(t.cop), na = t.cop == 7 || t.aop == 6 || t.aop == 7 ? 0 : operands(t.aop);
        for (int j = 0; j < 3; j++) {
            if (j >= nc) t.csrc[j] = t.cmod[j] = 0;
            if (j >= na) t.asrc[j] = t.amod[j] = 0;
        }
        if (t.cop == 7 || t.aop == 6 || t.aop == 7) t.aop = t.cop == 7 ? 7 : 6;
    }
    if (ds.lighting) {
        LutSel *sels[7] = {&ds.light.d0, &ds.light.d1, &ds.light.sp, &ds.light.fr, &ds.light.rb, &ds.light.rg, &ds.light.rr};
        for (LutSel *s : sels)
            if (!s->enabled) s->abs = false, s->input = 0;
    }
    return ds;
}

// the DrawState fields gen_fs() and build_pipeline() depend on, from a key (inverse of fs_key)
DrawState decode_key(const std::string &k) {
    DrawState ds{};
    size_t p = 0;
    auto get = [&]() -> u32 {
        u32 v = 0;
        if (p + 4 <= k.size()) memcpy(&v, k.data() + p, 4);
        p += 4;
        return v;
    };
    u32 w = get();
    ds.need_shade = w & 1;
    ds.wbuffer = w >> 1 & 1;
    ds.scissor_mode = (w >> 2 & 1) ? 1 : 0;
    ds.depth_bits = w >> 3 & 63;
    ds.color_write = w >> 9 & 1;
    if (!ds.need_shade) {
        ds.logic_op = (w >> 10 & 1) ? 3 : 0;  // reproduces hw_blend()
        return ds;
    }
    for (int i = 0; i < 3; i++) {
        w = get();
        TexUnitState &t = ds.tex[i];
        ds.need_tex[i] = w & 1;
        t.null_tex = w >> 1 & 1;
        t.type = w >> 2 & 15;
        t.coord = w >> 6 & 3;
        t.wrap_s = w >> 8 & 7;
        t.wrap_t = w >> 11 & 7;
        t.shadow_ortho = w >> 14 & 1;
    }
    ds.tev_count = std::min(get(), 6u);
    for (u32 i = 0; i < ds.tev_count; i++) {
        const u32 si = get() % 6;
        ds.tev_list[i] = (u8)si;
        TevStageState &t = ds.tev[si];
        for (int j = 0; j < 3; j++) {
            w = get();
            t.csrc[j] = w & 15;
            t.asrc[j] = w >> 4 & 15;
            t.cmod[j] = w >> 8 & 15;
            t.amod[j] = w >> 12 & 15;
        }
        w = get();
        t.cop = w & 15;
        t.aop = w >> 4 & 15;
        t.cmul = w >> 8 & 15;
        t.amul = w >> 12 & 15;
        t.upd_rgb = w >> 16 & 1;
        t.upd_a = w >> 17 & 1;
    }
    ds.lighting = get();
    if (ds.lighting) {
        LightingState &l = ds.light;
        w = get();
        l.num_lights = std::min(w & 15, 8u);
        l.config = w >> 4 & 15;
        l.bump_mode = w >> 8 & 3;
        l.bump_sel = w >> 10 & 3;
        l.shadow_sel = w >> 12 & 3;
        l.bump_renorm = w >> 14 & 1;
        l.shadow_enable = w >> 15 & 1;
        l.shadow_invert = w >> 16 & 1;
        l.shadow_primary = w >> 17 & 1;
        l.shadow_secondary = w >> 18 & 1;
        l.shadow_alpha = w >> 19 & 1;
        l.primary_alpha = w >> 20 & 1;
        l.secondary_alpha = w >> 21 & 1;
        l.clamp_highlights = w >> 22 & 1;
        LutSel *sels[7] = {&l.d0, &l.d1, &l.sp, &l.fr, &l.rb, &l.rg, &l.rr};
        for (auto *s : sels) {
            w = get();
            s->enabled = w & 1;
            s->abs = w >> 1 & 1;
            s->input = w >> 2 & 7;
        }
        for (u32 i = 0; i < l.num_lights; i++) {
            w = get();
            LightState &lt = l.lights[i];
            lt.num = w & 7;
            lt.directional = w >> 3 & 1;
            lt.two_sided = w >> 4 & 1;
            lt.geo0 = w >> 5 & 1;
            lt.geo1 = w >> 6 & 1;
            lt.dist_atten = w >> 7 & 1;
            lt.spot_atten = w >> 8 & 1;
            lt.shadow = w >> 9 & 1;
        }
    }
    w = get();
    ds.fog = w & 1;
    ds.fog_flip = w >> 1 & 1;
    ds.alpha_test = w >> 2 & 1;
    ds.alpha_func = w >> 3 & 7;
    w = get();
    ds.blend = w & 1;
    ds.eq_rgb = w >> 1 & 7;
    ds.eq_a = w >> 4 & 15;
    ds.sf_rgb = w >> 8 & 15;
    ds.df_rgb = w >> 12 & 15;
    ds.sf_a = w >> 16 & 15;
    ds.df_a = w >> 20 & 15;
    ds.logic_op = w >> 24 & 15;
    ds.color_mask = get();
    return ds;
}

