#pragma once

#include <string_view>

namespace bps::logging {
// Qt's own category (a "qt.*" one) is the platform narrating itself: it is worth a
// console line, not a line in engine.log. The app's own messages — no category, a
// "vgr.*" one, or a QML one — all belong to the one log.
inline bool QtMessageBelongsToApplication(const char* category) {
    return category == nullptr || std::string_view(category).rfind("qt.", 0) != 0;
}

inline bool QtMessageIsUserFacing(bool isQml, const char* category, std::string_view message) {
    // A QML warning/error is the app's own: always the user's business.
    if (isQml)
        return true;

    // A Qt category is Qt talking about itself, at any level: platform narration,
    // not an application error. Logged, never toasted.
    if (category != nullptr && std::string_view(category).rfind("qt.", 0) == 0)
        return false;

    if (message.find("DirectWrite: CreateFontFaceFromHDC") != std::string_view::npos)
        return false;

    // Everything else: the app's own qWarning() (no category, or a "vgr.*" one).
    return true;
}
} // namespace bps::logging