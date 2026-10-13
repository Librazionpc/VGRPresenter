#pragma once

// Which Qt messages may reach the USER as a notice, and which stay in engine.log.
//
// main.cpp's message handler publishes every warning/critical it sees as a toast
// ("Application — ..."). That is right for the app's own warnings — "NDI SendFrame
// FAILED", a media player error, a failed selftest step — and for every QML
// warning, because a QML binding error is the app's bug.
//
// It is wrong for Qt narrating the PLATFORM doing its job, which is what Qt's own
// logging categories are: "qt.qpa.clipboard", "qt.qpa.window" and friends are not
// about the app, are not caused by anything the user did, and are usually
// conditions Qt itself recovers from. The one that surfaced: reading the clipboard
// while another process holds it is NORMAL on Windows — the clipboard has a single
// owner at a time and every other app on the machine takes it when it copies — so
// Qt retries for a second and warns "Retrying to obtain clipboard." That arrived
// in the app as an amber "Application — Retrying to obtain clipboard." notice: the
// user could not act on it, nothing was wrong, and it taught them to ignore the
// notification area, which is the opposite of what it is for.
//
// WHY THIS LIVES HERE, AS A RULE RATHER THAN A STRING LIST IN THE HANDLER:
//   * it is by CATEGORY, not by message text — a new Qt line lands quiet
//     automatically, and there is no prefix list to keep in sync (the handler's
//     "vgr.*.progress" rule is written the same way, for the same reason);
//   * the handler is in main.cpp, which the unit tests do not compile, so a rule
//     living there cannot be checked — this one is (TestLogger).
//
// WHAT IT CAN NEVER HIDE: the app does its own OS-level clipboard work through the
// platform layer (WindowsClipboard), whose failures are Results that its callers
// report themselves; they do not travel as Qt messages. So silencing Qt's
// clipboard category cannot hide a real clipboard failure of ours.
//
// Qt messages are still logged (the handler writes engine.log before this rule
// runs) — a diagnosis loses nothing but gains a usable notification area.

#include <string_view>

namespace bps::logging {

// Should this Qt message be shown to the user (true) or only logged (false)?
// `isQml` is how main.cpp recognises a message from the QML engine (its category
// starts "qml", or its text starts with a URL); `category` is
// QLoggingCategory::categoryName() and may be null; `message` is the formatted
// message.
inline bool QtMessageIsUserFacing(bool isQml, const char* category, std::string_view message) {
    // A QML warning/error is the app's own: always the user's business.
    if (isQml)
        return true;

    // A Qt category is Qt talking about itself, at any level: platform narration,
    // not an application error. Logged, never toasted.
    if (category != nullptr && std::string_view(category).rfind("qt.", 0) == 0)
        return false;

    // One Qt line that arrives with NO category at all: its Windows font backend
    // probing a legacy system font ("Fixedsys") while resolving a monospace hint.
    // Harmless — the font that actually gets used renders fine.
    if (message.find("DirectWrite: CreateFontFaceFromHDC") != std::string_view::npos)
        return false;

    // Everything else: the app's own qWarning() (no category, or a "vgr.*" one).
    return true;
}

} // namespace bps::logging
