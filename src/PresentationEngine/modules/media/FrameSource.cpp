#include "modules/media/FrameSource.hpp"

namespace bps::media {

#ifdef _WIN32
std::shared_ptr<IFrameSource> MakeWindowsFrameSource();   // WindowsFrameSource.cpp
#endif

namespace {

// For platforms without a decoder yet: every request is Unsupported, so the UI keeps
// its placeholder tiles and nothing else breaks.
class UnsupportedFrameSource final : public IFrameSource {
public:
    Result<Picture> Grab(const std::string&, MediaKind, double, int) override {
        return Error::Make(Err::Unsupported, "FrameSource",
                           "no picture decoder is available on this platform yet");
    }
};

} // namespace

std::shared_ptr<IFrameSource> MakePlatformFrameSource() {
#ifdef _WIN32
    return MakeWindowsFrameSource();
#else
    return std::make_shared<UnsupportedFrameSource>();
#endif
}

} // namespace bps::media
