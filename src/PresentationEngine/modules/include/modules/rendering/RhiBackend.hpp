#pragma once

#include "modules/rendering/IGraphicsBackend.hpp"

#include <memory>
#include <string>

namespace bps::rendering {
// The cross-platform GPU backend, named the way the `render.backend` setting and the log name it.
inline constexpr const char* kRhiBackendName = "rhi";

// Builds the GPU backend on Qt's rendering hardware interface (QRhi): Direct3D 11 on Windows, Metal on
// macOS, Vulkan or OpenGL on Linux — ONE implementation of the engine's draw commands (the same
// shaders, the same pipelines, the same readback) for every OS. The Qt types live entirely in
// platform/qt/QtRhiBackend.cpp; nothing here includes a Qt header.
//
// Nullptr = unavailable (no usable GPU API, no Qt GUI application to host it, or a build without
// Qt Shader Tools); `failureReason` then says why, and the caller falls back to the software rasterizer.
std::unique_ptr<IGraphicsBackend> CreateRhiBackend(std::string* failureReason = nullptr);
} // namespace bps::rendering
