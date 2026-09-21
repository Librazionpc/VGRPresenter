#include "platform/windows/WinUtil.hpp"

#include <dxgi.h>

#include <algorithm>

namespace bps::platform::win {

namespace {

GpuInfo Query() {
    GpuInfo best;
    HMODULE lib = LoadLibraryW(L"dxgi.dll");
    if (!lib) return best;

    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create = ProcAddress<CreateFactory>(lib, "CreateDXGIFactory1");
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && (best.name.empty() || desc.DedicatedVideoMemory > best.vramBytes)) {
                best.name = Utf8(desc.Description);
                best.vramBytes = desc.DedicatedVideoMemory;
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(lib);
    return best;
}

} // namespace

const GpuInfo& PrimaryGpu() {
    static const GpuInfo info = Query();
    return info;
}

} // namespace bps::platform::win
