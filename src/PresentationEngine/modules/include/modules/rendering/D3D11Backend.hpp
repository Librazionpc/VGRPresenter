#pragma once

// D3D11 backend (docs/specs/17 §Backends) — the GPU rasterizer behind the same
// IGraphicsBackend seam the software one implements, so nothing above it
// changes: RenderEngine still compiles a scene into DrawCommands, still reads
// the frame back as RGBA8, and still hands the same Frame to the outputs, the
// preview and the NDI feed.
//
// What it buys the live loop: the per-frame raster happens on the GPU, and the
// readback (the one copy that has to come back to the CPU because the outputs,
// the recorder and the NDI sender all take RGBA8) is a device-to-CPU copy
// instead of a full CPU raster of every pixel.
//
// The FACTORY is the whole interface deliberately: no D3D11 type appears in any
// header, so the rendering module (and every translation unit that includes it)
// stays free of <d3d11.h>, and a build for a platform without D3D11 gets a
// nullptr from the same call rather than a compile error.
//
// Returns nullptr — never throws — when no device can be made: not Windows, no
// d3dcompiler (the HLSL is compiled at device creation), device creation
// failed for every driver type tried (hardware, then WARP, Microsoft's software
// rasterizer). `failureReason`, when given, receives a one-line explanation for
// the caller's log; the caller is expected to keep the software backend and say
// why. That is the fallback contract: the GPU is chosen when it is really
// there, and a machine without it runs exactly as it did before.

#include "modules/rendering/IGraphicsBackend.hpp"

#include <memory>
#include <string>

namespace bps::rendering {

// Kind asked for by the engine's `render.backend` setting (see RenderEngine).
// Kept here so the selection logic and the factory agree on the names.
inline constexpr const char* kD3D11BackendName = "directx11";

// Builds a D3D11 device + the HLSL it draws with. Nullptr = unavailable.
std::unique_ptr<IGraphicsBackend> CreateD3D11Backend(std::string* failureReason = nullptr);

} // namespace bps::rendering
