#version 440
// The QRhi backend's fragment shader: the D3D11 backend's PSMain (draw modes 0..2 and 8..10, the
// effect pass mode 3, the blend pass mode 4) in portable GLSL. Every formula is the D3D11 one: one
// picture on screen must be one picture on air, whichever rasterizer drew it.

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vLocal;
layout(location = 2) in vec2 vPix;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Draw {
    mat4 gClip;
    vec4 gRect;
    vec4 gUV;
    vec4 gColor;
    vec4 gParams;
    vec4 gFrame;
    vec4 gDir;
    vec4 gMisc;
};

layout(binding = 1) uniform sampler2D gTex;     // the picture / glyph atlas / effect source
layout(binding = 2) uniform sampler2D gBack;    // the blend pass's copy of the frame

// A copy of the frame is a render-target texture: bottom-up in memory under OpenGL, top-down elsewhere.
vec2 RtUv(vec2 uv)
{
    return vec2(uv.x, gMisc.x > 0.5 ? 1.0 - uv.y : uv.y);
}

// ---- the CSS blend functions (separated, then the four non-separable ones) ----
float BScreen(float b, float s)   { return b + s - b * s; }
float BOverlay(float b, float s)  { return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s); }
float BHardLight(float b, float s) { return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s); }
float BSoftLight(float b, float s)
{
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b)
                    : b + (2.0 * s - 1.0) * (d - b);
}
float BDodge(float b, float s)
{
    if (b <= 0.0) return 0.0;
    if (s >= 1.0) return 1.0;
    return min(1.0, b / (1.0 - s));
}
float BBurn(float b, float s)
{
    if (b >= 1.0) return 1.0;
    if (s <= 0.0) return 0.0;
    return 1.0 - min(1.0, (1.0 - b) / s);
}

float LumB(vec3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

vec3 ClipColorB(vec3 c)
{
    float l = LumB(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    vec3 o = c;
    if (n < 0.0 && l - n > 0.000001)
        o = l + (o - l) * l / (l - n);
    if (x > 1.0 && x - l > 0.000001)
        o = l + (o - l) * (1.0 - l) / (x - l);
    return o;
}

vec3 SetLumB(vec3 c, float l) { return ClipColorB(c + (l - LumB(c))); }

float SatB(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

vec3 SetSatB(vec3 c, float s)
{
    float lo = min(c.r, min(c.g, c.b));
    float hi = max(c.r, max(c.g, c.b));
    if (hi - lo <= 0.000001)
        return vec3(0.0, 0.0, 0.0);
    vec3 o;
    o.r = c.r >= hi ? s : (c.r <= lo ? 0.0 : (c.r - lo) * s / (hi - lo));
    o.g = c.g >= hi ? s : (c.g <= lo ? 0.0 : (c.g - lo) * s / (hi - lo));
    o.b = c.b >= hi ? s : (c.b <= lo ? 0.0 : (c.b - lo) * s / (hi - lo));
    return o;
}

// The ids are bps::rendering::BlendMode integers; Alpha, Additive and Multiply are real blend
// states and never reach this pass.
vec3 BlendFn(int m, vec3 cb, vec3 cs)
{
    if (m == 3)  return vec3(BScreen(cb.r, cs.r), BScreen(cb.g, cs.g), BScreen(cb.b, cs.b));
    if (m == 4)  return vec3(BOverlay(cb.r, cs.r), BOverlay(cb.g, cs.g), BOverlay(cb.b, cs.b));
    if (m == 5)  return vec3(BSoftLight(cb.r, cs.r), BSoftLight(cb.g, cs.g), BSoftLight(cb.b, cs.b));
    if (m == 6)  return vec3(BHardLight(cb.r, cs.r), BHardLight(cb.g, cs.g), BHardLight(cb.b, cs.b));
    if (m == 7)  return vec3(BDodge(cb.r, cs.r), BDodge(cb.g, cs.g), BDodge(cb.b, cs.b));
    if (m == 8)  return vec3(BBurn(cb.r, cs.r), BBurn(cb.g, cs.g), BBurn(cb.b, cs.b));
    if (m == 9)  return max(cb, cs);
    if (m == 10) return min(cb, cs);
    if (m == 11) return abs(cb - cs);
    if (m == 12) return cb + cs - 2.0 * cb * cs;                       // exclusion
    if (m == 13) return SetLumB(SetSatB(cs, SatB(cb)), LumB(cb));      // hue
    if (m == 14) return SetLumB(SetSatB(cb, SatB(cs)), LumB(cb));      // saturation
    if (m == 15) return SetLumB(cs, LumB(cb));                         // color
    if (m == 16) return SetLumB(cb, LumB(cs));                         // luminosity
    return cs;                                                         // normal
}

// Signed-distance coverage of a rounded rectangle: the same one-pixel anti-aliased edge as the CPU ramp.
float RoundedCoverage(float radius)
{
    float r = min(radius, 0.5 * min(gRect.z, gRect.w));
    if (r < 0.5)
        return 1.0;
    vec2 hs = gRect.zw * 0.5;
    vec2 q = abs(vLocal - hs) - (hs - r);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    return clamp(0.5 - d, 0.0, 1.0);
}

void main()
{
    int mode = int(gParams.w + 0.5);
    int effect = int(gParams.y + 0.5);
    float param = gParams.z;

    if (mode == 4)
    {
        // BLEND PASS: the object composited against a copy of the frame under it, written back as the
        // FINAL (premultiplied) pixel, so the pipeline blend state is a replace.
        int bm = int(gFrame.z + 0.5);
        int flags = int(gFrame.w + 0.5);

        // The target holds PREMULTIPLIED pixels; the blend maths is authored in STRAIGHT space.
        vec4 b = texture(gBack, RtUv(vPix / gFrame.xy));
        float ab = b.a;
        vec3 Cb = ab > 0.0001 ? b.rgb / ab : b.rgb;

        vec4 s = gColor;
        if (mod(float(flags), 2.0) > 0.5)               // flag 1: the source is a texture
        {
            vec4 t = texture(gTex, vUv);
            s = vec4(t.rgb * gColor.rgb, t.a * gColor.a);
        }

        float cov = 1.0;
        if (flags >= 2)                                 // flag 2: rounded-rect coverage
            cov = RoundedCoverage(gParams.x);

        float as_ = s.a;
        vec3 Cs = s.rgb;
        vec3 B = BlendFn(bm, Cb, Cs);
        float ao = as_ + ab * (1.0 - as_);
        vec3 co = as_ * (1.0 - ab) * Cs + as_ * ab * B + (1.0 - as_) * ab * Cb;
        fragColor = mix(vec4(b.rgb, ab), vec4(co, ao), cov);
        return;
    }

    if (mode == 3)
    {
        // EFFECT PASS: samples a scratch copy of the frame over the scissored region.
        if (effect == 0)
        {
            // Box blur, one axis per pass: the same separable average the CPU box filter computes.
            int taps = int(clamp(gDir.z, 1.0, 64.0));
            vec3 acc = vec3(0.0);
            float accA = 0.0;
            for (int k = -64; k <= 64; ++k)
            {
                if (k < -taps || k > taps)
                    continue;
                vec4 q = texture(gTex, RtUv(vUv + gDir.xy * float(k) / gFrame.xy));
                acc += q.rgb;
                accA += q.a;
            }
            float n = 2.0 * float(taps) + 1.0;
            // Already premultiplied: blurring premultiplied pixels IS blurring the composite.
            fragColor = vec4(acc / n, accA / n);
            return;
        }

        // The effect parameters are authored against STRAIGHT pixels: un-premultiply, transform,
        // re-premultiply, so a brightness or a saturation means the same number as on the CPU.
        vec4 p = texture(gTex, RtUv(vUv));
        float a = p.a;
        vec3 s = a > 0.0001 ? p.rgb / a : p.rgb;
        if (effect == 3)
            a *= clamp(param, 0.0, 1.0);                                              // Opacity
        else if (effect == 4)
            s = min(s * param, vec3(1.0));                                            // Brightness
        else if (effect == 5)
            s = clamp((s - 0.5) * param + 0.5, 0.0, 1.0);                             // Contrast
        else if (effect == 6)
        {
            float l = dot(s, vec3(0.2126, 0.7152, 0.0722));
            s = clamp(mix(vec3(l, l, l), s, param), 0.0, 1.0);                        // Saturation
        }
        fragColor = vec4(s * a, a);
        return;
    }

    // Draw modes 0..2 (solid, textured, rounded rect); 8..10 are the same shapes with the colour
    // inverted, the form the MULTIPLY blend needs.
    bool invert = mode >= 8;
    int base = invert ? mode - 8 : mode;

    vec4 outC = gColor;
    if (base == 1)
    {
        // Blit / glyph: the source texel times the tint (the glyph atlas is white; every on-air text
        // colour arrives as the tint).
        vec4 t = texture(gTex, vUv);
        outC = vec4(t.rgb * gColor.rgb, t.a * gColor.a);
    }
    else if (base == 2)
    {
        outC.a *= RoundedCoverage(gParams.x);
    }

    if (invert)
        outC = vec4(1.0 - outC.rgb, outC.a);
    fragColor = outC;
}
