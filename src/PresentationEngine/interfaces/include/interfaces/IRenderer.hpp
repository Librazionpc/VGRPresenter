#pragma once

#include "interfaces/IService.hpp"

namespace bps {

// Renderer interface (core/rendering/). Implementations may be OpenGL, Vulkan,
// or software backends; modules only ever see this interface (00 §2).
class IRenderer : public IService {
public:
    ~IRenderer() override = default;

    virtual Result<void> BeginFrame() = 0;
    virtual Result<void> EndFrame() = 0;
    virtual Result<void> Present() = 0;
};

} // namespace bps
