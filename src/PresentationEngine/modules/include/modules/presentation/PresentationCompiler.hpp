#pragma once

// PresentationCompiler (docs/specs/19 §Compiler). Before going live the
// presentation is compiled into an optimized runtime representation: validates
// references, resolves assets, prepares scenes, preloads transitions, builds
// caches. The live runtime executes compiled data, not raw files.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::presentation {

// One compiled slide: resolved scene id + index + transition plan.
struct CompiledSlide {
    std::string slideId;
    std::string sceneId;            // rendering scene id (built by SceneBuilder)
    size_t index = 0;               // visible index in the compiled presentation
    TransitionKind transitionIn = TransitionKind::Fade;
    double transitionMs = 500.0;
    double durationMs = 0.0;
};

// The compiled runtime form of a presentation.
struct CompiledPresentation {
    bool ok = false;
    std::string presentationId;
    std::vector<CompiledSlide> slides;
    std::map<std::string, size_t, std::less<>> indexById;   // slideId -> index
    size_t warnings = 0;
    size_t errors = 0;
};

class PresentationCompiler {
public:
    // Compiles a presentation into its runtime representation. `sceneIds`
    // (optional) maps slideId -> prepared scene id; when absent, deterministic
    // scene ids ("pres:<presentationId>:<slideId>") are generated so the live
    // runtime can build scenes lazily.
    CompiledPresentation Compile(const Presentation& presentation,
                                 const std::vector<ValidationIssue>& validation,
                                 const std::map<std::string, std::string, std::less<>>* sceneIds =
                                     nullptr) const;
};

} // namespace bps::presentation
