#pragma once

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