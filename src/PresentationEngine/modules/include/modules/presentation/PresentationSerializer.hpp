#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <string_view>

namespace bps::presentation {
class IPresentationSerializer {
public:
    virtual ~IPresentationSerializer() = default;
    virtual Result<std::string> Serialize(const Presentation& presentation) const = 0;
    virtual Result<Presentation> Deserialize(std::string_view json) const = 0;
};

class PresentationSerializer final : public IPresentationSerializer {
public:
    static constexpr int kSchemaVersion = 1;

    Result<std::string> Serialize(const Presentation& presentation) const override;
    Result<Presentation> Deserialize(std::string_view json) const override;

    Result<std::string> SerializeTemplate(const SlideTemplate& tmpl) const;
    Result<SlideTemplate> DeserializeTemplate(std::string_view json) const;
};
} // namespace bps::presentation