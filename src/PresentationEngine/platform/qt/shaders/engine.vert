#version 440
// The QRhi backend's one vertex shader: a quad described by constants, drawn as a 4-vertex strip.
// This is the D3D11 backend's VSMain, rewritten to be portable: the corner comes from a 4-vertex
// buffer (no SV_VertexID: GLSL ES 100 has no gl_VertexID), and the frame-pixel position is handed to
// the fragment shader as a varying instead of being read from gl_FragCoord/SV_Position, whose origin
// differs between APIs.

layout(location = 0) in vec2 corner;            // (0,0) (1,0) (0,1) (1,1): top-left origin

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec2 vLocal;           // destination pixels from the quad origin
layout(location = 2) out vec2 vPix;             // destination pixels in the FRAME, y down

layout(std140, binding = 0) uniform Draw {
    mat4 gClip;      // QRhi::clipSpaceCorrMatrix(): authored in GL clip space, corrected per API
    vec4 gRect;      // x, y, w, h  (destination pixels)
    vec4 gUV;        // u0, v0, u1, v1
    vec4 gColor;     // straight fill colour / texture tint
    vec4 gParams;    // x = corner radius, y = effect id, z = effect param, w = mode
    vec4 gFrame;     // x = frame width, y = frame height, z/w = blend pass: mode, flags
    vec4 gDir;       // xy = one blur step, z = blur radius
    vec4 gMisc;      // x = 1 when a render-target copy is stored bottom-up (OpenGL)
};

void main()
{
    vec2 px = gRect.xy + corner * gRect.zw;
    vec4 ndc = vec4(px.x / gFrame.x * 2.0 - 1.0, 1.0 - px.y / gFrame.y * 2.0, 0.0, 1.0);
    gl_Position = gClip * ndc;
    vUv = mix(gUV.xy, gUV.zw, corner);
    vLocal = corner * gRect.zw;
    vPix = px;
}
