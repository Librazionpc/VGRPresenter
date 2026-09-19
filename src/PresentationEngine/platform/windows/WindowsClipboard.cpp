#include "platform/windows/WindowsClipboard.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>   // DROPFILES lives here, not in shellapi.h

#include <cwchar>
#include <vector>

namespace bps::platform {

namespace {
// Plain Error, not Result<void> — this is returned from functions with
// several different Result<T> return types (string, vector<string>, void),
// and Error converts implicitly to any of them; a concrete Result<void>
// would only convert to itself.
Error ClipboardError(std::string_view op) {
    return Error::Make(Err::IoError, "Clipboard",
                       std::string(op) + " failed: " + win::LastErrorString(GetLastError()));
}
} // namespace

Result<std::string> WindowsClipboard::ReadText() const {
    if (!OpenClipboard(nullptr)) return ClipboardError("OpenClipboard");
    std::string text;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h)))
                text = win::Utf8(w);
            if (h) GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return text;
}

Result<void> WindowsClipboard::WriteText(std::string_view text) {
    if (!OpenClipboard(nullptr)) return ClipboardError("OpenClipboard");
    if (!EmptyClipboard()) {
        CloseClipboard();
        return ClipboardError("EmptyClipboard");
    }
    std::wstring wide = win::Wide(text);
    size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return ClipboardError("GlobalAlloc");
    }
    void* dst = GlobalLock(h);
    if (!dst) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("GlobalLock");
    }
    memcpy(dst, wide.c_str(), bytes);
    GlobalUnlock(h);
    if (SetClipboardData(CF_UNICODETEXT, h) == nullptr) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("SetClipboardData");
    }
    CloseClipboard();
    return Ok();
}

Result<std::vector<std::string>> WindowsClipboard::GetFiles() const {
    std::vector<std::string> files;
    if (!OpenClipboard(nullptr)) return ClipboardError("OpenClipboard");
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            HDROP drop = static_cast<HDROP>(h);
            UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring path(len, L'\0');
                ::DragQueryFileW(drop, i, path.data(), len + 1);
                files.push_back(win::Utf8(path));
            }
        }
    }
    CloseClipboard();
    return files;
}

Result<void> WindowsClipboard::SetFiles(const std::vector<std::string>& paths) {
    if (!OpenClipboard(nullptr)) return ClipboardError("OpenClipboard");
    if (!EmptyClipboard()) {
        CloseClipboard();
        return ClipboardError("EmptyClipboard");
    }

    // DROPFILES header + double-null-terminated UTF-16 path list.
    size_t bytes = sizeof(DROPFILES);
    std::vector<std::wstring> wide;
    wide.reserve(paths.size());
    for (const auto& p : paths) {
        std::wstring w = win::Wide(p);
        wide.push_back(w);
        bytes += (w.size() + 1) * sizeof(wchar_t);
    }
    bytes += sizeof(wchar_t);   // final terminator
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        CloseClipboard();
        return ClipboardError("GlobalAlloc");
    }
    char* dst = static_cast<char*>(GlobalLock(h));
    if (!dst) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("GlobalLock");
    }
    DROPFILES* df = reinterpret_cast<DROPFILES*>(dst);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* cursor = reinterpret_cast<wchar_t*>(dst + sizeof(DROPFILES));
    for (const auto& w : wide) {
        std::wcscpy(cursor, w.c_str());
        cursor += w.size() + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(h);
    if (SetClipboardData(CF_HDROP, h) == nullptr) {
        GlobalFree(h);
        CloseClipboard();
        return ClipboardError("SetClipboardData");
    }
    CloseClipboard();
    return Ok();
}

} // namespace bps::platform
