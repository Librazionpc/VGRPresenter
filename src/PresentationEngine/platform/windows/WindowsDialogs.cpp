#include "platform/windows/WindowsDialogs.hpp"

#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace bps::platform {

namespace {

// Per-call COM apartment; uninitializes on scope exit even when the caller
// already initialized (we own the matching CoUninitialize for our
// CoInitializeEx, which is the correct pairing).
class ComApartment {
public:
    ComApartment() { hr_ = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComApartment() {
        if (hr_ == S_OK || hr_ == S_FALSE) ::CoUninitialize();
    }
    bool ok() const { return hr_ == S_OK || hr_ == S_FALSE; }

private:
    HRESULT hr_ = E_FAIL;
};

std::vector<COMDLG_FILTERSPEC> ToFilterSpec(const std::vector<std::string>& filters) {
    std::vector<COMDLG_FILTERSPEC> spec;
    std::vector<std::wstring> names, patterns;
    for (const auto& f : filters) {
        // Accept "Display Name (*.ext;*.ext)" entries and split them.
        size_t open = f.find('(');
        size_t close = f.find(')', open);
        if (open != std::string::npos && close != std::string::npos && close > open + 1) {
            names.push_back(win::Wide(f.substr(0, open)));
            std::string pat = f.substr(open + 1, close - open - 1);
            // Remove the leading "*" so "*.png;*.jpg" becomes "*.png;*.jpg"
            // (Windows accepts "*.png;*.jpg" directly).
            patterns.push_back(win::Wide(pat));
        } else {
            names.push_back(win::Wide(f));
            patterns.push_back(L"*.*");
        }
    }
    for (size_t i = 0; i < names.size(); ++i)
        spec.push_back({names[i].c_str(), patterns[i].c_str()});
    return spec;
}

Result<std::optional<std::string>> RunCommonDialog(REFCLSID clsid, DWORD options,
                                                   const std::wstring& title,
                                                   const std::wstring& defaultName,
                                                   const std::vector<COMDLG_FILTERSPEC>& specs) {
    ComApartment com;
    if (!com.ok())
        return Error::Make(Err::IoError, "Dialogs", "COM initialization failed");
    IFileDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg));
    if (FAILED(hr))
        return Error::Make(Err::IoError, "Dialogs", "dialog creation failed");
    if (!title.empty()) dlg->SetTitle(title.c_str());
    if (!specs.empty()) dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    if (!defaultName.empty()) dlg->SetFileName(defaultName.c_str());
    hr = dlg->SetOptions(options);
    if (SUCCEEDED(hr)) hr = dlg->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dlg->Release();
        return std::optional<std::string>{};   // user cancelled
    }
    if (FAILED(hr)) {
        dlg->Release();
        return Error::Make(Err::IoError, "Dialogs", "dialog failed");
    }
    IShellItem* item = nullptr;
    hr = dlg->GetResult(&item);
    dlg->Release();
    if (FAILED(hr) || !item)
        return Error::Make(Err::IoError, "Dialogs", "dialog returned no result");
    PWSTR path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(hr) || !path) return std::optional<std::string>{};
    std::wstring ws(path);
    ::CoTaskMemFree(path);
    // Result<optional<string>> needs an optional<string> value, not the
    // bare string win::Utf8() returns — Result<T> converts from a plain T,
    // but T here IS optional<string>, so it has to be wrapped explicitly.
    return std::optional<std::string>{win::Utf8(ws)};
}

} // namespace

Result<std::optional<std::string>> WindowsDialogs::OpenFileDialog(
    std::string_view title, const std::vector<std::string>& filters) {
    std::vector<COMDLG_FILTERSPEC> specs = ToFilterSpec(filters);
    return RunCommonDialog(CLSID_FileOpenDialog,
                           FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST,
                           win::Wide(title), L"", specs);
}

Result<std::optional<std::string>> WindowsDialogs::SaveFileDialog(
    std::string_view title, std::string_view defaultName) {
    return RunCommonDialog(CLSID_FileSaveDialog,
                           FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT,
                           win::Wide(title), win::Wide(defaultName), {});
}

Result<std::optional<std::string>> WindowsDialogs::SelectFolderDialog(std::string_view title) {
    return RunCommonDialog(CLSID_FileOpenDialog,
                           FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS,
                           win::Wide(title), L"", {});
}

Result<void> WindowsDialogs::MessageDialog(std::string_view title, std::string_view message) {
    int rc = ::MessageBoxW(nullptr, win::Wide(message).c_str(), win::Wide(title).c_str(),
                           MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (rc == 0) return Error::Make(Err::IoError, "Dialogs", "MessageBoxW failed");
    return Ok();
}

Result<bool> WindowsDialogs::QuestionDialog(std::string_view title, std::string_view message) {
    int rc = ::MessageBoxW(nullptr, win::Wide(message).c_str(), win::Wide(title).c_str(),
                           MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (rc == 0) return Error::Make(Err::IoError, "Dialogs", "MessageBoxW failed");
    return rc == IDYES;
}

} // namespace bps::platform
