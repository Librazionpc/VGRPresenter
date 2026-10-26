#version 440

// ---------------------------------------------------------------------------
// slidefilter.frag — the edit canvas's preview of a slide's filter set
// ---------------------------------------------------------------------------
// Compiled by qsb at build time (see the qt_add_shaders call in CMakeLists.txt,
// resource path qrc:/shaders/slidefilter.frag.qsb) and used as a layered item's
// effect through qml/components/SlideFilterEffect.qml. The values, their units
// and the OP ORDER are the engine's — rendering::SlideFilterValues and
// Effects.cpp's ApplyFilters — because the whole point of this shader is that
// the canvas shows the same picture the HDMI/NDI output is fed, not an
// approximation of it.
//
// `source` is bound by Qt itself: when a ShaderEffect is a layered item's
// effect, the layer's own texture is that sampler (Qt Quick's "Layered Items"
// contract), and it arrives PREMULTIPLIED — hence the divide/multiply by alpha
// around the colour maths below.
//
// Blur runs first and that is not an approximation: the six colour operations
// are linear per-pixel maps and blur is a linear spatial one, so they commute.
// Doing the spatial pass first therefore yields exactly the CSS order
// (hue, invert, blur, grayscale, brightness, contrast, saturate) at one
// texture-fetch set instead of two passes.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

// One uniform block, as Qt 6 requires. qt_Matrix and qt_Opacity must be the
// first two members (the built-in vertex shader shares this block), then this
// effect's own uniforms, matched to SlideFilterEffect.qml's properties by name.
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float hueRotate;        // degrees, 0–360
    float invertAmount;     // 0–1
    float blurPx;           // stage pixels, 0–100
    float grayAmount;       // 0–1
    float sepiaAmount;      // 0–1 (0 on the slide pass — sepia is a MEDIA item's
                            // own filter, same row FreeShow's media filters list)
    float brightnessValue;  // multiplier, 1 = identity
    float contrastValue;    // multiplier, 1 = identity
    float saturationValue;  // multiplier, 1 = identity
    float stageW;           // the canvas size in stage pixels — blur is
    float stageH;           // specified in those, not in texture pixels
};

layout(binding = 1) uniform sampler2D source;

// Rec. 709 luma — the same weights Effects.cpp's matrices use, so a grayscale
// preview cannot come out at a different tone than the output.
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// CSS filter hue-rotate(deg): the published 3x3 matrix, applied as row dot
// products (exactly the matrix HueRotateMatrix() builds on the engine side).
vec3 hueRotateColor(vec3 c, float deg) {
    float a = radians(deg);
    float s = sin(a);
    float co = cos(a);
    vec3 r0 = vec3(0.213 + co * 0.787 - s * 0.213,
                   0.715 - co * 0.715 - s * 0.715,
                   0.072 - co * 0.072 + s * 0.928);
    vec3 r1 = vec3(0.213 - co * 0.213 + s * 0.143,
                   0.715 + co * 0.285 + s * 0.140,
                   0.072 - co * 0.072 - s * 0.283);
    vec3 r2 = vec3(0.213 - co * 0.213 - s * 0.787,
                   0.715 - co * 0.715 + s * 0.715,
                   0.072 + co * 0.928 + s * 0.072);
    return clamp(vec3(dot(c, r0), dot(c, r1), dot(c, r2)), 0.0, 1.0);
}

void main() {
    vec2 uv = qt_TexCoord0;
    vec4 c = texture(source, uv);

    // Box-blur preview: nine taps in a 3x3 grid whose spacing IS the requested
    // radius, weighted toward the centre. The engine's BoxBlur averages the same
    // neighbourhood, so the canvas reads as the same amount of blur without one
    // shader permutation per radius.
    if (blurPx > 0.5) {
        vec2 stepUV = 0.5 * vec2(blurPx / max(stageW, 1.0), blurPx / max(stageH, 1.0));
        c  = texture(source, uv) * 0.20;
        c += texture(source, uv + vec2(stepUV.x, 0.0)) * 0.11;
        c += texture(source, uv - vec2(stepUV.x, 0.0)) * 0.11;
        c += texture(source, uv + vec2(0.0, stepUV.y)) * 0.11;
        c += texture(source, uv - vec2(0.0, stepUV.y)) * 0.11;
        c += texture(source, uv + stepUV) * 0.09;
        c += texture(source, uv - stepUV) * 0.09;
        c += texture(source, uv + vec2(stepUV.x, -stepUV.y)) * 0.09;
        c += texture(source, uv + vec2(-stepUV.x, stepUV.y)) * 0.09;
    }

    // Premultiplied in, premultiplied out — the layer's texture is premultiplied
    // and so must the result be. Skipping this is what bleaches the soft edges
    // of every text glyph and rounded corner on the canvas.
    if (c.a > 0.0001)
        c.rgb /= c.a;

    if (hueRotate != 0.0)
        c.rgb = hueRotateColor(c.rgb, hueRotate);
    if (invertAmount > 0.0)
        c.rgb = mix(c.rgb, vec3(1.0) - c.rgb, invertAmount);
    if (grayAmount > 0.0)
        c.rgb = mix(c.rgb, vec3(dot(c.rgb, LUMA)), grayAmount);

    // sepia(amount): the CSS/FreeShow matrix, mixed in — the same op the engine's
    // ApplyMediaFilters runs on the pixels it sends to the outputs.
    if (sepiaAmount > 0.0) {
        vec3 s = vec3(dot(c.rgb, vec3(0.393, 0.769, 0.189)),
                      dot(c.rgb, vec3(0.349, 0.686, 0.168)),
                      dot(c.rgb, vec3(0.272, 0.534, 0.131)));
        c.rgb = mix(c.rgb, clamp(s, 0.0, 1.0), sepiaAmount);
    }
    if (brightnessValue != 1.0)
        c.rgb = c.rgb * brightnessValue;
    if (contrastValue != 1.0)
        c.rgb = (c.rgb - vec3(0.5)) * contrastValue + vec3(0.5);
    if (saturationValue != 1.0)
        c.rgb = mix(vec3(dot(c.rgb, LUMA)), c.rgb, saturationValue);

    c.rgb = clamp(c.rgb, 0.0, 1.0) * c.a;
    fragColor = c * qt_Opacity;
}
