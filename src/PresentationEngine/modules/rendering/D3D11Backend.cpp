#include "core/common/StageTiming.hpp"
#include "modules/rendering/D3D11Backend.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)

#include <d3d11.h>
#include <d3dcompiler.h>

namespace bps::rendering {
namespace {
// ---------------------------------------------------------------------------
// Minimal COM ownership (no WRL dependency: this file builds with the same
// MinGW toolchain as the rest of the engine).
// ---------------------------------------------------------------------------
template <typename T>
class Com {
public:
    Com() = default;
    explicit Com(T* raw) : p_(raw) {}
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ~Com() { Reset(); }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    // Hands the reference over (no AddRef): the caller becomes the owner.
    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    void AddRef() {
        if (p_)
            p_->AddRef();
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() {
        Reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
constexpr const char* kShaderSource = R"HLSL(
cbuffer DrawCB : register(b0)
{
    float4 gRect;    // x, y, w, h  (destination pixels)
    float4 gUV;      // u0, v0, u1, v1
    float4 gColor;   // straight fill colour / texture tint
    float4 gParams;  // x = corner radius, y = effect id, z = effect param, w = mode
    float4 gFrame;   // x = frame width, y = frame height, z/w = blend pass: mode, flags
    float4 gDir;     // xy = one blur step / blend pass: region origin, z = blur radius
};

Texture2D gTex : register(t0);
// THE BACKDROP, for the blend pass only (mode 4): a copy of the frame as it was
// before this object drew. The blend unit cannot read where it is writing, so a
// CSS blend mode (screen, overlay, soft light, hue…) has to be computed per
// pixel against this copy — exactly what the software rasterizer does with its
// destination pixels.
Texture2D gBack : register(t1);
SamplerState gSamp : register(s0);

// ---- the CSS blend functions (separated, then the four non-separable ones) ---
// Each matches SoftwareGraphicsBackend's BlendPixel term for term: one picture
// on screen must be one picture on air, whichever rasterizer drew it.
float BMultiply(float b, float s) { return b * s; }
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

float LumB(float3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

float3 ClipColorB(float3 c)
{
    float l = LumB(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    float3 o = c;
    if (n < 0.0 && l - n > 0.000001)
        o = l + (o - l) * l / (l - n);
    if (x > 1.0 && x - l > 0.000001)
        o = l + (o - l) * (1.0 - l) / (x - l);
    return o;
}

float3 SetLumB(float3 c, float l) { return ClipColorB(c + (l - LumB(c))); }

float SatB(float3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

// Rank the channels, not their VALUES: the biggest gets s, the smallest 0, the
// middle one keeps its distance in between (the spec's SetSat). Compared against
// the original channels so the result lands back on the right component.
float3 SetSatB(float3 c, float s)
{
    float lo = min(c.r, min(c.g, c.b));
    float hi = max(c.r, max(c.g, c.b));
    if (hi - lo <= 0.000001)
        return float3(0.0, 0.0, 0.0);
    float3 o;
    o.r = c.r >= hi ? s : (c.r <= lo ? 0.0 : (c.r - lo) * s / (hi - lo));
    o.g = c.g >= hi ? s : (c.g <= lo ? 0.0 : (c.g - lo) * s / (hi - lo));
    o.b = c.b >= hi ? s : (c.b <= lo ? 0.0 : (c.b - lo) * s / (hi - lo));
    return o;
}

// The blend mode ids are bps::rendering::BlendMode's integers. Alpha, Additive
// and Multiply never arrive here — they are real blend states on this backend,
// and NeedsBlendPass() keeps them off this pass.
float3 BlendFn(int m, float3 cb, float3 cs)
{
    if (m == 3)  return float3(BScreen(cb.r, cs.r), BScreen(cb.g, cs.g), BScreen(cb.b, cs.b));
    if (m == 4)  return float3(BOverlay(cb.r, cs.r), BOverlay(cb.g, cs.g), BOverlay(cb.b, cs.b));
    if (m == 5)  return float3(BSoftLight(cb.r, cs.r), BSoftLight(cb.g, cs.g), BSoftLight(cb.b, cs.b));
    if (m == 6)  return float3(BHardLight(cb.r, cs.r), BHardLight(cb.g, cs.g), BHardLight(cb.b, cs.b));
    if (m == 7)  return float3(BDodge(cb.r, cs.r), BDodge(cb.g, cs.g), BDodge(cb.b, cs.b));
    if (m == 8)  return float3(BBurn(cb.r, cs.r), BBurn(cb.g, cs.g), BBurn(cb.b, cs.b));
    if (m == 9)  return max(cb, cs);
    if (m == 10) return min(cb, cs);
    if (m == 11) return abs(cb - cs);
    if (m == 12) return cb + cs - 2.0 * cb * cs;                       // exclusion
    if (m == 13) return SetLumB(SetSatB(cs, SatB(cb)), LumB(cb));      // hue
    if (m == 14) return SetLumB(SetSatB(cb, SatB(cs)), LumB(cb));      // saturation
    if (m == 15) return SetLumB(cs, LumB(cb));                        // color
    if (m == 16) return SetLumB(cb, LumB(cs));                        // luminosity
    return cs;                                                        // normal
}

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float2 local : TEXCOORD1;   // destination pixels from the quad's origin
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 corners[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    float2 t = corners[vid];
    float2 px = gRect.xy + t * gRect.zw;
    VSOut o;
    o.pos = float4(px.x / gFrame.x * 2.0 - 1.0, 1.0 - px.y / gFrame.y * 2.0, 0.0, 1.0);
    o.uv = lerp(gUV.xy, gUV.zw, t);
    o.local = t * gRect.zw;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    int mode = (int)(gParams.w + 0.5);
    int effect = (int)(gParams.y + 0.5);
    float param = gParams.z;

    if (mode == 4)
    {
        // BLEND PASS: the object's picture composited against a copy of the
        // frame underneath it, written back as the FINAL pixel (premultiplied),
        // so the blend state is a replace. gFrame.z = blend mode id,
        // gFrame.w = flags (1 textured source, 2 rounded rect).
        int bm = (int)(gFrame.z + 0.5);
        int flags = (int)(gFrame.w + 0.5);

        // The target holds PREMULTIPLIED pixels (see the blend states); the
        // blend maths is authored in STRAIGHT space, so un-premultiply first.
        // The backdrop copy holds this quad's REGION at the same frame-sized
        // coordinates, so the sample is the pixel itself.
        float4 b = gBack.Sample(gSamp, (i.pos.xy - gDir.xy) / gFrame.xy);
        float ab = b.a;
        float3 Cb = ab > 0.0001 ? b.rgb / ab : b.rgb;

        float4 s = gColor;
        if ((flags & 1) != 0)
        {
            float4 t = gTex.Sample(gSamp, i.uv);
            s = float4(t.rgb * gColor.rgb, t.a * gColor.a);
        }

        // Coverage: a rounded rectangle fades at its own anti-aliased edge, and
        // where it does not cover, the backdrop must come back UNTOUCHED.
        float cov = 1.0;
        if ((flags & 2) != 0)
        {
            float r = min(gParams.x, 0.5 * min(gRect.z, gRect.w));
            if (r >= 0.5)
            {
                float2 half = gRect.zw * 0.5;
                float2 q = abs(i.local - half) - (half - r);
                float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
                cov = saturate(0.5 - d);
            }
        }

        float as_ = s.a;
        float3 Cs = s.rgb;
        float3 B = BlendFn(bm, Cb, Cs);
        float ao = as_ + ab * (1.0 - as_);
        // co = as(1-ab)Cs + as·ab·B(Cb,Cs) + (1-as)ab·Cb — the same formula the
        // software rasterizer runs (it comes out premultiplied, which is what
        // this target stores).
        float3 co = as_ * (1.0 - ab) * Cs + as_ * ab * B + (1.0 - as_) * ab * Cb;
        return lerp(float4(b.rgb, ab), float4(co, ao), cov);
    }

    if (mode == 3)
    {
        // EFFECT PASS: the frame was copied into a scratch texture and this pass
        // samples that copy, transforms it over the scissored region and writes
        // it back.
        if (effect == 0)
        {
            // Box blur, one axis per pass (the caller runs horizontal, then
            // vertical): the same separable average the CPU box filter computes.
            int taps = (int)clamp(gDir.z, 1.0, 64.0);
            float3 acc = 0.0;
            float accA = 0.0;
            for (int k = -64; k <= 64; ++k)
            {
                if (k < -taps || k > taps)
                    continue;
                float4 q = gTex.Sample(gSamp, i.uv + gDir.xy * (float)k / gFrame.xy);
                acc += q.rgb;
                accA += q.a;
            }
            float n = 2.0 * (float)taps + 1.0;
            // Already premultiplied: blurring premultiplied pixels IS blurring
            // the composited result, so no un/re-premultiply on this path.
            return float4(acc / n, accA / n);
        }

        // The target holds PREMULTIPLIED pixels (see the blend states), while the
        // effect parameters are authored against STRAIGHT pixels: un-premultiply,
        // transform, re-premultiply — that is what makes a brightness or a
        // saturation mean the same number here as on the CPU rasterizer.
        float4 p = gTex.Sample(gSamp, i.uv);
        float a = p.a;
        float3 s = a > 0.0001 ? p.rgb / a : p.rgb;
        if (effect == 3)
        {
            a *= saturate(param);                     // Opacity
        }
        else if (effect == 4)
        {
            s = min(s * param, 1.0);                  // Brightness
        }
        else if (effect == 5)
        {
            s = saturate((s - 0.5) * param + 0.5);    // Contrast
        }
        else if (effect == 6)
        {
            float l = dot(s, float3(0.2126, 0.7152, 0.0722));
            s = saturate(lerp(float3(l, l, l), s, param));   // Saturation
        }
        return float4(s * a, a);
    }

    // Draw modes 0..2 (solid, textured, rounded rect); 8..10 are the same shapes
    // with the colour inverted, which is the form the MULTIPLY blend needs.
    bool invert = mode >= 8;
    int base = invert ? mode - 8 : mode;

    float4 outC = gColor;
    if (base == 1)
    {
        // Blit / glyph: the source texel times the tint — RGB and alpha, the same
        // full tint the CPU path applies (the glyph atlas is white and every
        // on-air text colour arrives this way).
        float4 t = gTex.Sample(gSamp, i.uv);
        outC = float4(t.rgb * gColor.rgb, t.a * gColor.a);
    }
    else if (base == 2)
    {
        // Rounded rectangle: the signed distance from the rounded boundary gives
        // the same one-pixel anti-aliased edge as the CPU coverage ramp.
        float r = min(gParams.x, 0.5 * min(gRect.z, gRect.w));
        if (r >= 0.5)
        {
            float2 half = gRect.zw * 0.5;
            float2 q = abs(i.local - half) - (half - r);
            float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
            outC.a *= saturate(0.5 - d);
        }
    }

    if (invert)
        return float4(1.0 - outC.rgb, outC.a);
    return outC;
}
)HLSL";

// The constant buffer's rows (6 × 16 bytes).
struct DrawConstants {
    float rect[4];
    float uv[4];
    float color[4];
    float params[4];
    float frame[4];
    float dir[4];
};
static_assert(sizeof(DrawConstants) % 16 == 0, "constant buffer rows must be 16-byte aligned");

// Straight RGBA8 → the premultiplied float the target composites in (the clear
// is the one command that bypasses the blend unit).
void PremultipliedClear(const Color& c, float out[4]) {
    out[0] = c.r * c.a;
    out[1] = c.g * c.a;
    out[2] = c.b * c.a;
    out[3] = c.a;
}

// Premultiplied target byte → the straight RGBA8 the engine hands downstream.
uint32_t Unpremultiply(uint32_t px) {
    const uint32_t a = (px >> 24) & 0xFFu;
    if (a == 0u || a == 255u)
        return px;   // fully transparent, or already straight
    auto straight = [a](uint32_t v) -> uint32_t {
        const uint32_t s = (v * 255u + a / 2u) / a;
        return s > 255u ? 255u : s;
    };
    return (a << 24) | (straight((px >> 16) & 0xFFu) << 16) | (straight((px >> 8) & 0xFFu) << 8) |
           straight((px >> 0) & 0xFFu);
}
} // namespace

// ===========================================================================
// D3D11GraphicsBackend
// ===========================================================================
class D3D11GraphicsBackend final : public IGraphicsBackend {
public:
    D3D11GraphicsBackend(ID3D11Device* device, ID3D11DeviceContext* context)
        : device_(device), context_(context) {
        device_.AddRef();
        context_.AddRef();
    }

    ~D3D11GraphicsBackend() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ReleaseGpu();
    }

    const BackendCapabilities& Capabilities() const noexcept override { return caps_; }

    // --- Texture management -------------------------------------------------
    Result<TextureId> CreateTexture(const RgbaImage& image) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (image.empty())
            return Error::Make(Err::InvalidArgument, "D3D11", "empty texture");
        Texture t;
        if (!UploadTexture(image, t))
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "texture upload failed");
        const TextureId id{nextTexture_++};
        // The CPU copy is kept for ReadTexture (thumbnails, capture) and is what
        // TextureMemoryBytes() accounts, exactly like the software backend.
        t.cpu = image;
        textureBytes_ += t.cpu.ByteSize();
        textures_.emplace(id.id, std::move(t));
        return id;
    }

    Result<void> UpdateTexture(TextureId id, const RgbaImage& image) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = textures_.find(id.id);
        if (it == textures_.end())
            return Error::Make(Err::Render_TextureNotFound, "D3D11", "no texture");
        if (image.empty() || !context_ || !it->second.tex)
            return Ok();
        if (it->second.cpu.width != image.width || it->second.cpu.height != image.height) {
            Texture fresh;
            if (!UploadTexture(image, fresh))
                return Error::Make(Err::Render_BackendUnavailable, "D3D11",
                                   "texture re-upload failed after a size change");
            textureBytes_ -= it->second.cpu.ByteSize();
            it->second = std::move(fresh);
            it->second.cpu = image;
            textureBytes_ += it->second.cpu.ByteSize();
            return Ok();
        }
        context_->UpdateSubresource(it->second.tex.Get(), 0, nullptr, image.pixels.data(),
                                    static_cast<UINT>(image.width) * 4u, 0);
        textureBytes_ -= it->second.cpu.ByteSize();
        it->second.cpu = image;
        textureBytes_ += it->second.cpu.ByteSize();
        return Ok();
    }

    Result<void> DestroyTexture(TextureId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = textures_.find(id.id);
        if (it == textures_.end())
            return Error::Make(Err::Render_TextureNotFound, "D3D11", "no texture");
        textureBytes_ -= it->second.cpu.ByteSize();
        textures_.erase(it);
        return Ok();
    }

    RgbaImage ReadTexture(TextureId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = textures_.find(id.id);
        return it == textures_.end() ? RgbaImage{} : it->second.cpu;
    }

    uint64_t TextureMemoryBytes() const noexcept override { return textureBytes_; }

    // --- Shaders (recorded, and compiled where the source is HLSL) ----------
    Result<uint32_t> CreateShader(std::string_view name, std::string_view source) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint32_t id = nextShader_++;
        shaders_[id] = std::string(source);
        (void)name;
        if (!source.empty() && device_) {
            Com<ID3DBlob> blob;
            Com<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                                          "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                          blob.Put(), errors.Put());
            if (SUCCEEDED(hr))
                compiledShaders_.emplace(id, std::move(blob));
        }
        return id;
    }

    Result<void> DestroyShader(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        compiledShaders_.erase(id);
        shaders_.erase(id);
        return Ok();
    }

    size_t ShaderCount() const noexcept override { return shaders_.size(); }

    // --- Frame lifecycle ----------------------------------------------------
    Result<void> BeginFrame(int width, int height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (width <= 0 || height <= 0)
            return Error::Make(Err::InvalidArgument, "D3D11", "bad frame size");
        if (!context_)
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "no context");
        if (!EnsureFrame(width, height))
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "frame targets");
        pending_ = PendingEffect{};
        return Ok();
    }

    Result<void> Submit(const std::vector<DrawCommand>& commands) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_ || !rtv)
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "no render target");
        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        for (const DrawCommand& cmd : commands)
            Execute(cmd);
        // Unbind before returning: the frame may not be sampled (effects) or
        // copied (readback) while it is still attached as a render target.
        ID3D11ShaderResourceView* none[1] = {nullptr};
        context_->PSSetShaderResources(0, 1, none);
        ID3D11RenderTargetView* out[1] = {nullptr};
        context_->OMSetRenderTargets(1, out, nullptr);
        return Ok();
    }

    Result<RgbaImage> Readback() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_ || !rt_ || !staging)
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "no frame to read");
        static timing::StageTiming stageTiming("d3d11 readback");
        const auto tCopy = std::chrono::steady_clock::now();
        context_->CopyResource(staging.Get(), rt_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return Error::Make(Err::Render_BackendUnavailable, "D3D11", "staging map failed");
        stageTiming.Add("copy+map (waits for the GPU)", timing::MsSince(tCopy));
        const auto tConvert = std::chrono::steady_clock::now();
        RgbaImage img;
        img.width = frameWidth_;
        img.height = frameHeight_;
        img.pixels.resize(static_cast<size_t>(frameWidth_) * static_cast<size_t>(frameHeight_));
        // BULK-COPY the mapped rows first, convert in ordinary memory afterwards: reading the mapped
        // staging memory pixel by pixel measured 75 ms for a 1080p frame (8 MB at ~100 MB/s); a row
        // memcpy brings it to ~20 ms and the per-pixel conversion then runs on cached memory.
        // (SSE4.1 streaming loads were tried and measured SLOWER here: the mapping is not uncached.)
        const auto* base = static_cast<const uint8_t*>(mapped.pData);
        const size_t rowBytes = static_cast<size_t>(frameWidth_) * sizeof(uint32_t);
        for (int y = 0; y < frameHeight_; ++y)
            std::memcpy(reinterpret_cast<uint8_t*>(img.pixels.data()) + static_cast<size_t>(y) * rowBytes,
                        base + static_cast<size_t>(mapped.RowPitch) * y, rowBytes);
        context_->Unmap(staging.Get(), 0);   // the GPU's copy is free again before the CPU work below
        for (uint32_t& px : img.pixels) {
            const uint32_t a = px >> 24;
            if (a != 0u && a != 255u)
                px = Unpremultiply(px);
        }
        stageTiming.Add("convert", timing::MsSince(tConvert));
        stageTiming.EndUnit();
        return img;
    }

    Result<void> Present() override { return Ok(); }   // offscreen: nothing to present
    Result<void> EndFrame() override { return Ok(); }

    Result<void> Shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ReleaseGpu();
        return Ok();
    }

    bool InitPipeline(std::string* why) {
        if (!device_ || !context_) {
            if (why) *why = "no device";
            return false;
        }

        auto compileFailure = [why](ID3DBlob* errors, const char* entry) {
            if (!why) return;
            *why = std::format("the {} HLSL did not compile", entry);
            if (errors) {
                const char* text = static_cast<const char*>(errors->GetBufferPointer());
                if (text)
                    *why += std::format(": {}", std::string(text, errors->GetBufferSize()));
            }
        };

        Com<ID3DBlob> vsBlob;
        Com<ID3DBlob> psBlob;
        Com<ID3DBlob> errors;
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "engine-shaders", nullptr,
                              nullptr, "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                              vsBlob.Put(), errors.Put()))) {
            (void)compileFailure;
            compileFailure(errors.Get(), "vertex");
            return false;
        }
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "engine-shaders", nullptr,
                              nullptr, "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                              psBlob.Put(), errors.Put()))) {
            compileFailure(errors.Get(), "pixel");
            return false;
        }

        if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                               vsBlob->GetBufferSize(), nullptr, vs.Put()))) {
            if (why) *why = "CreateVertexShader failed";
            return false;
        }
        if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                              psBlob->GetBufferSize(), nullptr, ps.Put()))) {
            if (why) *why = "CreatePixelShader failed";
            return false;
        }
        // An EMPTY input layout still has to exist: the vertex shader reads
        // SV_VertexID, but a draw needs a layout bound.
        D3D11_INPUT_ELEMENT_DESC none{};
        if (FAILED(device_->CreateInputLayout(&none, 0, vsBlob->GetBufferPointer(),
                                              vsBlob->GetBufferSize(), layout.Put()))) {
            if (why) *why = "CreateInputLayout failed";
            return false;
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(DrawConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, constantBuffer.Put()))) {
            if (why) *why = "constant buffer creation failed";
            return false;
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sd, sampler.Put()))) {
            if (why) *why = "sampler creation failed";
            return false;
        }

        auto makeBlend = [&](bool enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op,
                             D3D11_BLEND srcA, D3D11_BLEND dstA, D3D11_BLEND_OP opA,
                             Com<ID3D11BlendState>& out) {
            D3D11_BLEND_DESC desc{};
            desc.RenderTarget[0].BlendEnable = enable ? TRUE : FALSE;
            desc.RenderTarget[0].SrcBlend = src;
            desc.RenderTarget[0].DestBlend = dst;
            desc.RenderTarget[0].BlendOp = op;
            desc.RenderTarget[0].SrcBlendAlpha = srcA;
            desc.RenderTarget[0].DestBlendAlpha = dstA;
            desc.RenderTarget[0].BlendOpAlpha = opA;
            desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            return SUCCEEDED(device_->CreateBlendState(&desc, out.Put()));
        };

        if (!makeBlend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                       D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, blendAlpha)) {
            if (why) *why = "alpha blend state failed";
            return false;
        }
        // ADDITIVE — dst + src·a, alpha = max(da, a).
        if (!makeBlend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                       D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, blendAdd)) {
            if (why) *why = "additive blend state failed";
            return false;
        }
        // MULTIPLY — the CPU's dst·(1 − a·(1−src)): the shader emits (1−src, a)
        // and REV_SUBTRACT turns that into exactly that formula; alpha = max.
        if (!makeBlend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT,
                       D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, blendMul)) {
            if (why) *why = "multiply blend state failed";
            return false;
        }
        // EFFECT PASS — a replace: a region transform must not blend with
        // itself.
        if (!makeBlend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
                       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, blendNone)) {
            if (why) *why = "replace blend state failed";
            return false;
        }

        // Fixed pipeline state: no culling (a quad is drawn either way round), no
        // depth (2D engine), scissoring on (the effect regions).
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rd, raster.Put()))) {
            if (why) *why = "rasterizer state failed";
            return false;
        }

        context_->IASetInputLayout(layout.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vs.Get(), nullptr, 0);
        context_->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11SamplerState* samplers[1] = {sampler.Get()};
        context_->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer* cbs[1] = {constantBuffer.Get()};
        context_->VSSetConstantBuffers(0, 1, cbs);
        context_->PSSetConstantBuffers(0, 1, cbs);
        context_->RSSetState(raster.Get());
        return true;
    }

private:
    struct Texture {
        Com<ID3D11Texture2D> tex;
        Com<ID3D11ShaderResourceView> srv;
        RgbaImage cpu;
    };

    struct PendingEffect {
        bool active = false;
        Rect region;
        int effect = 0;
        float param = 0.0f;
    };

    bool UploadTexture(const RgbaImage& image, Texture& out) {
        if (!device_)
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(image.width);
        desc.Height = static_cast<UINT>(image.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = image.pixels.data();
        data.SysMemPitch = static_cast<UINT>(image.width) * 4u;
        if (FAILED(device_->CreateTexture2D(&desc, &data, out.tex.Put())))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        return SUCCEEDED(device_->CreateShaderResourceView(out.tex.Get(), &srvDesc, out.srv.Put()));
    }

    bool MakeTarget(int width, int height, Com<ID3D11Texture2D>& tex,
                    Com<ID3D11ShaderResourceView>* srv) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, tex.Put())))
            return false;
        if (!srv)
            return true;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = desc.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        return SUCCEEDED(device_->CreateShaderResourceView(tex.Get(), &srvd, srv->Put()));
    }

    bool EnsureFrame(int width, int height) {
        if (rt_ && frameWidth_ == width && frameHeight_ == height)
            return true;
        ReleaseFrame();
        frameWidth_ = width;
        frameHeight_ = height;

        if (!MakeTarget(width, height, rt_, nullptr))
            return false;
        if (FAILED(device_->CreateRenderTargetView(rt_.Get(), nullptr, rtv.Put())))
            return false;
        // The scratch copies an effect pass reads from (the target itself may not
        // be sampled while it is being written).
        if (!MakeTarget(width, height, scratchA, &scratchASrv))
            return false;
        if (!MakeTarget(width, height, scratchB, &scratchBSrv))
            return false;

        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = static_cast<UINT>(width);
        sd.Height = static_cast<UINT>(height);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, staging.Put())))
            return false;

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        context_->ClearRenderTargetView(rtv.Get(), transparent);
        SetScissor(Rect(0, 0, static_cast<float>(width), static_cast<float>(height)));
        return true;
    }

    void ReleaseFrame() {
        rt_.Reset();
        rtv.Reset();
        scratchA.Reset();
        scratchASrv.Reset();
        scratchB.Reset();
        scratchBSrv.Reset();
        staging.Reset();
    }

    void ReleaseGpu() {
        if (context_) {
            ID3D11ShaderResourceView* none[1] = {nullptr};
            context_->PSSetShaderResources(0, 1, none);
            ID3D11RenderTargetView* out[1] = {nullptr};
            context_->OMSetRenderTargets(1, out, nullptr);
            context_->ClearState();
        }
        ReleaseFrame();
        textures_.clear();
        compiledShaders_.clear();
        shaders_.clear();
        textureBytes_ = 0;
        vs.Reset();
        ps.Reset();
        layout.Reset();
        constantBuffer.Reset();
        sampler.Reset();
        raster.Reset();
        blendAlpha.Reset();
        blendAdd.Reset();
        blendMul.Reset();
        blendNone.Reset();
        context_.Reset();
        device_.Reset();
    }

    void SetScissor(const Rect& region) {
        if (!context_)
            return;
        D3D11_RECT sc{};
        sc.left = std::clamp(static_cast<int>(region.x), 0, frameWidth_);
        sc.top = std::clamp(static_cast<int>(region.y), 0, frameHeight_);
        sc.right = std::clamp(static_cast<int>(region.Right()), 0, frameWidth_);
        sc.bottom = std::clamp(static_cast<int>(region.Bottom()), 0, frameHeight_);
        if (sc.right < sc.left)
            sc.right = sc.left;
        if (sc.bottom < sc.top)
            sc.bottom = sc.top;
        context_->RSSetScissorRects(1, &sc);
    }

    void SetBlend(BlendMode mode) {
        ID3D11BlendState* state = blendAlpha.Get();
        if (mode == BlendMode::Additive)
            state = blendAdd.Get();
        else if (mode == BlendMode::Multiply)
            state = blendMul.Get();
        context_->OMSetBlendState(state, nullptr, 0xFFFFFFFFu);
    }

    // One quad = one command. `mode` follows the shader's numbering (0 solid,
    // 1 textured, 2 rounded rect; +8 for the multiply-inverted variant).
    void DrawQuad(int mode, const Rect& rect, const Rect& uv, const Color& color, float radius,
                  ID3D11ShaderResourceView* srv, BlendMode blend, float effect, float param) {
        DrawConstants cb{};
        cb.rect[0] = rect.x;
        cb.rect[1] = rect.y;
        cb.rect[2] = rect.width;
        cb.rect[3] = rect.height;
        cb.uv[0] = uv.x;
        cb.uv[1] = uv.y;
        cb.uv[2] = uv.x + uv.width;
        cb.uv[3] = uv.y + uv.height;
        cb.color[0] = color.r;
        cb.color[1] = color.g;
        cb.color[2] = color.b;
        cb.color[3] = color.a;
        cb.params[0] = radius;
        cb.params[1] = effect;
        cb.params[2] = param;
        cb.params[3] = static_cast<float>(mode);
        cb.frame[0] = static_cast<float>(frameWidth_);
        cb.frame[1] = static_cast<float>(frameHeight_);
        context_->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &cb, 0, 0);
        context_->PSSetShaderResources(0, 1, &srv);
        SetBlend(blend);
        context_->Draw(4, 0);
    }

    static bool NeedsBlendPass(BlendMode m) {
        return m != BlendMode::Alpha && m != BlendMode::Additive && m != BlendMode::Multiply;
    }

    // shape: 0 = solid, 1 = textured, 2 = rounded rect (the shader's numbering).
    void BlendQuad(int shape, const Rect& rect, const Rect& uv, const Color& color, float radius,
                   ID3D11ShaderResourceView* srv, BlendMode blend) {
        if (!context_)
            return;
        if (!scratchB || !scratchBSrv || !rt_) {
            // No backdrop copy to blend against: draw it as a plain picture here
            // rather than leaving a hole in the frame.
            DrawQuad(shape, rect, uv, color, radius, srv, BlendMode::Alpha, 0.0f, 0.0f);
            return;
        }
        const int x0 = std::clamp(static_cast<int>(rect.x), 0, frameWidth_);
        const int y0 = std::clamp(static_cast<int>(rect.y), 0, frameHeight_);
        const int x1 = std::clamp(static_cast<int>(rect.Right()), 0, frameWidth_);
        const int y1 = std::clamp(static_cast<int>(rect.Bottom()), 0, frameHeight_);
        if (x1 <= x0 || y1 <= y0)
            return;
        // A texture cannot be a copy DESTINATION while it is bound as a shader
        // resource, and the previous blend pass left this one bound on slot 1.
        ID3D11ShaderResourceView* none[1] = {nullptr};
        context_->PSSetShaderResources(1, 1, none);
        const D3D11_BOX box{static_cast<UINT>(x0), static_cast<UINT>(y0), 0u,
                            static_cast<UINT>(x1), static_cast<UINT>(y1), 1u};
        context_->CopySubresourceRegion(scratchB.Get(), 0, 0, 0, 0, rt_.Get(), 0, &box);

        DrawConstants cb{};
        cb.rect[0] = rect.x;
        cb.rect[1] = rect.y;
        cb.rect[2] = rect.width;
        cb.rect[3] = rect.height;
        cb.uv[0] = uv.x;
        cb.uv[1] = uv.y;
        cb.uv[2] = uv.x + uv.width;
        cb.uv[3] = uv.y + uv.height;
        cb.color[0] = color.r;
        cb.color[1] = color.g;
        cb.color[2] = color.b;
        cb.color[3] = color.a;
        cb.params[0] = radius;
        cb.params[3] = 4.0f;   // mode: blend pass
        cb.frame[0] = static_cast<float>(frameWidth_);
        cb.frame[1] = static_cast<float>(frameHeight_);
        cb.frame[2] = static_cast<float>(static_cast<int>(blend));   // blend mode id
        // flags: 1 = the source is a texture, 2 = rounded-rect coverage.
        cb.frame[3] = static_cast<float>((srv ? 1 : 0) | (shape == 2 ? 2 : 0));
        // The backdrop copy sits at the frame's own coordinates, so the shader
        // needs the region's origin to sample it.
        cb.dir[0] = static_cast<float>(x0);
        cb.dir[1] = static_cast<float>(y0);
        context_->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11ShaderResourceView* srvs[2] = {srv, scratchBSrv.Get()};
        context_->PSSetShaderResources(0, 2, srvs);
        // The shader already produced the FINAL pixel, so this is a replace —
        // blending again would composite the result with itself.
        context_->OMSetBlendState(blendNone.Get(), nullptr, 0xFFFFFFFFu);
        // Write only this quad's pixels; the backdrop everywhere else stays.
        SetScissor(rect);
        context_->Draw(4, 0);
        SetScissor(rect_full());
    }

    void Execute(const DrawCommand& cmd) {
        const Rect full(0, 0, static_cast<float>(frameWidth_), static_cast<float>(frameHeight_));
        switch (cmd.type) {
            case DrawCommand::Type::Clear: {
                float premul[4];
                PremultipliedClear(cmd.color, premul);
                if (rtv)
                    context_->ClearRenderTargetView(rtv.Get(), premul);
                break;
            }
            case DrawCommand::Type::SetBlend:
                break;   // the blend travels on each command (cmd.blend)
            case DrawCommand::Type::FillRect: {
                SetScissor(full);
                // EDGE-SNAPPED (RenderTypes' SnapRectToPixelEdges) — the CPU
                // path's rule, so abutting fills share one pixel boundary.
                const Rect r = SnapRectToPixelEdges(cmd.rect);
                const BlendMode blend = static_cast<BlendMode>(cmd.blend);
                if (NeedsBlendPass(blend)) {
                    BlendQuad(0, r, Rect(0, 0, 0, 0), cmd.color, 0.0f, nullptr, blend);
                    break;
                }
                DrawQuad(cmd.blend == static_cast<int>(BlendMode::Multiply) ? 8 : 0,
                         r, Rect(0, 0, 0, 0), cmd.color, 0.0f, nullptr,
                         blend, 0.0f, 0.0f);
                break;
            }
            case DrawCommand::Type::BlitImage:
            case DrawCommand::Type::DrawGlyph: {
                auto it = textures_.find(cmd.texture.id);
                if (it == textures_.end()) {
                    static std::once_flag once;
                    std::call_once(once, [] {
                        Logger::Instance().Warning(
                            "blit: a texture id this backend does not hold — nothing drawn",
                            "Graphics");
                    });
                    break;
                }
                const RgbaImage& src = it->second.cpu;
                // A glyph lives in a shared atlas and carries its own cell in
                // srcRect; a blit without one means the whole image.
                Rect uv = cmd.srcRect;
                if (uv.width <= 0.0f || uv.height <= 0.0f)
                    uv = Rect(0, 0, static_cast<float>(src.width), static_cast<float>(src.height));
                uv.x /= static_cast<float>(src.width);
                uv.y /= static_cast<float>(src.height);
                uv.width /= static_cast<float>(src.width);
                uv.height /= static_cast<float>(src.height);

                const Rect dst(static_cast<float>(static_cast<int>(cmd.rect.x)),
                               static_cast<float>(static_cast<int>(cmd.rect.y)),
                               static_cast<float>(static_cast<int>(cmd.rect.width)),
                               static_cast<float>(static_cast<int>(cmd.rect.height)));

                SetScissor(full);
                const BlendMode blend = static_cast<BlendMode>(cmd.blend);
                if (NeedsBlendPass(blend)) {
                    // The picture's mode (the MEDIA tab's Blend dropdown) is
                    // computed against the frame underneath it.
                    BlendQuad(1, dst, uv, cmd.color, 0.0f, it->second.srv.Get(), blend);
                    break;
                }
                const int mode = (cmd.blend == static_cast<int>(BlendMode::Multiply) ? 8 : 0) + 1;
                DrawQuad(mode, dst, uv, cmd.color, 0.0f, it->second.srv.Get(),
                         blend, 0.0f, 0.0f);
                break;
            }
            case DrawCommand::Type::DrawShape: {
                if (cmd.color.a <= 0.0f)
                    break;
                SetScissor(full);
                const Rect r = SnapRectToPixelEdges(cmd.rect);
                const BlendMode blend = static_cast<BlendMode>(cmd.blend);
                const int shape = cmd.cornerRadius >= 0.5f ? 2 : 0;
                if (NeedsBlendPass(blend)) {
                    BlendQuad(shape, r, Rect(0, 0, 0, 0), cmd.color, cmd.cornerRadius, nullptr,
                              blend);
                    break;
                }
                DrawQuad(shape, r, Rect(0, 0, 0, 0), cmd.color, cmd.cornerRadius, nullptr, blend,
                         0.0f, 0.0f);
                break;
            }
            case DrawCommand::Type::PushEffect:
                pending_.active = true;
                pending_.region = cmd.rect;
                pending_.effect = cmd.effect;
                pending_.param = cmd.effectParam;
                break;
            case DrawCommand::Type::PopEffect:
                if (pending_.active) {
                    ApplyEffect(pending_.effect, pending_.param, pending_.region);
                    pending_ = PendingEffect{};
                }
                break;
        }
    }

    void EffectPass(int effect, float param, float radius, float dirX, float dirY,
                    ID3D11ShaderResourceView* src) {
        DrawConstants cb{};
        cb.rect[2] = static_cast<float>(frameWidth_);
        cb.rect[3] = static_cast<float>(frameHeight_);
        cb.uv[2] = 1.0f;
        cb.uv[3] = 1.0f;
        cb.color[0] = cb.color[1] = cb.color[2] = cb.color[3] = 1.0f;
        cb.params[1] = static_cast<float>(effect);
        cb.params[2] = param;
        cb.params[3] = 3.0f;   // mode: effect pass
        cb.frame[0] = static_cast<float>(frameWidth_);
        cb.frame[1] = static_cast<float>(frameHeight_);
        cb.dir[0] = dirX;
        cb.dir[1] = dirY;
        cb.dir[2] = radius;
        context_->UpdateSubresource(constantBuffer.Get(), 0, nullptr, &cb, 0, 0);
        context_->PSSetShaderResources(0, 1, &src);
        context_->OMSetBlendState(blendNone.Get(), nullptr, 0xFFFFFFFFu);   // replace, not blend
        context_->Draw(4, 0);
    }

    void ApplyEffect(int effect, float param, const Rect& region) {
        if (!scratchA || !scratchB || region.width <= 0.0f || region.height <= 0.0f)
            return;
        const bool blur = (effect == 0);
        // The ids the compositor implements; the others are composite operations
        // upstream, which the CPU rasterizer ignores too.
        if (!blur && effect != 3 && effect != 4 && effect != 5 && effect != 6)
            return;

        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        // The scissor limits the WRITE; the blur's taps still read outside the
        // region, exactly like the CPU filter averaging neighbours beyond it.
        SetScissor(region);

        if (blur) {
            // Separable box blur: horizontal, then vertical from a copy of that
            // result (the CPU's square (2r+1)² average at a fraction of the taps).
            context_->CopyResource(scratchA.Get(), rt_.Get());
            EffectPass(0, param, static_cast<float>(std::clamp(param, 1.0f, 64.0f)), 1.0f, 0.0f,
                       scratchASrv.Get());
            context_->CopyResource(scratchB.Get(), rt_.Get());
            EffectPass(0, param, static_cast<float>(std::clamp(param, 1.0f, 64.0f)), 0.0f, 1.0f,
                       scratchBSrv.Get());
        } else {
            context_->CopyResource(scratchA.Get(), rt_.Get());
            EffectPass(effect, param, 0.0f, 0.0f, 0.0f, scratchASrv.Get());
        }

        SetScissor(rect_full());
    }

    Rect rect_full() const {
        return Rect(0, 0, static_cast<float>(frameWidth_), static_cast<float>(frameHeight_));
    }

    BackendCapabilities caps_{kD3D11BackendName, true, 16384, 0, true, true, true};

    mutable std::mutex mutex_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;

    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> constantBuffer;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11BlendState> blendAlpha;
    Com<ID3D11BlendState> blendAdd;
    Com<ID3D11BlendState> blendMul;
    Com<ID3D11BlendState> blendNone;

    Com<ID3D11Texture2D> rt_;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11Texture2D> scratchA;
    Com<ID3D11ShaderResourceView> scratchASrv;
    Com<ID3D11Texture2D> scratchB;
    Com<ID3D11ShaderResourceView> scratchBSrv;
    Com<ID3D11Texture2D> staging;

    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint32_t, Com<ID3DBlob>> compiledShaders_;
    std::map<uint32_t, std::string> shaders_;
    uint32_t nextTexture_ = 1;
    uint32_t nextShader_ = 1;
    uint64_t textureBytes_ = 0;

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    PendingEffect pending_;
};
} // namespace bps::rendering

// ===========================================================================
// Factory
// ===========================================================================
namespace bps::rendering {
std::unique_ptr<IGraphicsBackend> CreateD3D11Backend(std::string* failureReason) {
    auto fail = [failureReason](std::string why) -> std::unique_ptr<IGraphicsBackend> {
        if (failureReason)
            *failureReason = std::move(why);
        return nullptr;
    };

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = E_FAIL;
    D3D_DRIVER_TYPE used = D3D_DRIVER_TYPE_HARDWARE;
    for (D3D_DRIVER_TYPE type : types) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                               device.Put(), &got, context.Put());
        if (SUCCEEDED(hr)) {
            used = type;
            break;
        }
    }
    if (FAILED(hr) || !device || !context)
        return fail(
            std::format("D3D11CreateDevice failed (hr=0x{:08x})", static_cast<unsigned>(hr)));

    auto backend = std::make_unique<D3D11GraphicsBackend>(device.Get(), context.Get());
    std::string why;
    if (!backend->InitPipeline(&why))
        return fail(why.empty() ? std::string("GPU pipeline setup failed") : why);
    // The backend took its own reference; the locals release theirs as they go.

    Logger::Instance().Info(
        std::format("D3D11 backend ready ({} driver, feature level 0x{:x}; frames come back to the "
                    "CPU through a staging copy)",
                    used == D3D_DRIVER_TYPE_WARP ? "WARP software" : "hardware",
                    static_cast<unsigned>(got)),
        "Graphics");
    return backend;
}
} // namespace bps::rendering

#else   // !_WIN32

namespace bps::rendering {
std::unique_ptr<IGraphicsBackend> CreateD3D11Backend(std::string* failureReason) {
    if (failureReason)
        failureReason->clear();   // not Windows: there is nothing to explain (the QRhi backend is the GPU path here)
    return nullptr;
}
} // namespace bps::rendering

#endif  // _WIN32