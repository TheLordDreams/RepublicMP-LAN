// SPDX-License-Identifier: GPL-3.0-only
#include "frame_capture.h"
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>

namespace {
std::wstring s_pluginDir, s_gameDir;
ULONGLONG s_nextPoll = 0;
bool s_failedRequest = false;

template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { Close(); }
    void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
};
struct Mapping {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~Mapping() { context->Unmap(texture, 0); }
};
std::wstring Directory(HMODULE module)
{
    wchar_t file[32768];
    DWORD n = GetModuleFileNameW(module, file, _countof(file));
    if (!n || n >= _countof(file)) return {};
    std::wstring path(file, n);
    auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}
bool Exists(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
bool WriteAll(HANDLE file, const void* data, DWORD size)
{
    const BYTE* p = static_cast<const BYTE*>(data);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, p, size, &written, nullptr) || !written) return false;
        size -= written;
        p += written;
    }
    return true;
}
HRESULT IoError()
{
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
}
// All paths are constants relative to the positively identified loaded DLL.
void ReportFailure(HRESULT result, const char* stage)
{
    s_failedRequest = true; // leave request in place, but never retry it each frame
    char message[256];
    int n = _snprintf_s(message, sizeof(message), _TRUNCATE,
        "FAIL: %s (HRESULT 0x%08lX)\r\nRemove the screenshot.request marker before retrying.\r\n",
        stage, static_cast<unsigned long>(result));
    File file;
    file.h = CreateFileW((s_pluginDir + L"RepublicMP.qa.screenshot.error").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.h != INVALID_HANDLE_VALUE && n > 0) WriteAll(file.h, message, static_cast<DWORD>(n));
    Logf("republicmp  QA FRAME: %s (HRESULT 0x%08lX)", stage, static_cast<unsigned long>(result));
}
HRESULT Capture(IDXGISwapChain* swapChain, const std::wstring& temporary, const char** stage)
{
    *stage = "validate game swapchain";
    if (!swapChain) return E_POINTER;
    DXGI_SWAP_CHAIN_DESC chain = {};
    HRESULT hr = swapChain->GetDesc(&chain);
    if (FAILED(hr)) return hr;
    DWORD owner = 0;
    if (!chain.OutputWindow || !GetWindowThreadProcessId(chain.OutputWindow, &owner) ||
        owner != GetCurrentProcessId()) return E_ACCESSDENIED;

    *stage = "get game backbuffer";
    Com<ID3D11Texture2D> source;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source.p));
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC desc = {};
    source.p->GetDesc(&desc);
    bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    *stage = "validate framebuffer format and dimensions (RGBA8/BGRA8, no MSAA, <=8192)";
    if ((!rgba && !bgra) || !desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192 ||
        desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1) return E_INVALIDARG;

    *stage = "get D3D11 device";
    Com<ID3D11Device> device;
    source.p->GetDevice(&device.p);
    // ID3D11Resource::GetDevice returns void; a null device is still rejected.
    if (!device.p) return E_NOINTERFACE;
    Com<ID3D11DeviceContext> context;
    device.p->GetImmediateContext(&context.p);
    if (!context.p) return E_NOINTERFACE;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    *stage = "create staging texture";
    hr = device.p->CreateTexture2D(&desc, nullptr, &staging.p);
    if (FAILED(hr)) return hr;
    context.p->CopyResource(staging.p, source.p);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    *stage = "map copied framebuffer";
    hr = context.p->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    Mapping mapping{context.p, staging.p};
    DWORD rowBytes = desc.Width * 4;
    if (!mapped.pData || mapped.RowPitch < rowBytes) return E_UNEXPECTED;

    BITMAPFILEHEADER header = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(desc.Width);
    info.biHeight = -static_cast<LONG>(desc.Height); // top-down, matching D3D row order
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = rowBytes * desc.Height; // <=256 MiB, checked above
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + info.biSizeImage;
    File output;
    *stage = "write temporary BMP";
    output.h = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.h == INVALID_HANDLE_VALUE) return IoError();
    if (!WriteAll(output.h, &header, sizeof(header)) || !WriteAll(output.h, &info, sizeof(info))) return IoError();
    std::vector<BYTE> row(rowBytes);
    for (UINT y = 0; y < desc.Height; ++y) {
        const BYTE* src = static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            row[4*x] = src[4*x + (rgba ? 2 : 0)];
            row[4*x+1] = src[4*x+1];
            row[4*x+2] = src[4*x + (rgba ? 0 : 2)];
            row[4*x+3] = 255;
        }
        if (!WriteAll(output.h, row.data(), rowBytes)) return IoError();
    }
    if (!FlushFileBuffers(output.h)) return IoError();
    output.Close();
    return S_OK;
}
}

void QaFrameCapture_Bind(const TsmHost* host)
{
    TsmBind(host);
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&QaFrameCapture_Bind), &self)) return;
    s_pluginDir = Directory(self);
    s_gameDir = Directory(nullptr);
}

void QaFrameCapture_Tick(IDXGISwapChain* swapChain)
{
    ULONGLONG now = GetTickCount64();
    if (now < s_nextPoll) return;
    s_nextPoll = now + 250;
    if (s_pluginDir.empty() || s_gameDir.empty() ||
        !Exists(s_gameDir + L"TEST-COPY.txt") || !Exists(s_pluginDir + L"RepublicMP.qa.enabled")) return;
    std::wstring request = s_pluginDir + L"RepublicMP.qa.screenshot.request";
    if (!Exists(request)) { s_failedRequest = false; return; }
    if (s_failedRequest) return;
    std::wstring temporary = s_pluginDir + L"RepublicMP.qa.frame.bmp.tmp";
    const char* stage = "capture framebuffer";
    HRESULT hr = E_FAIL;
    try { hr = Capture(swapChain, temporary, &stage); }
    catch (...) { stage = "allocate screenshot buffers"; hr = E_OUTOFMEMORY; }
    if (SUCCEEDED(hr)) {
        stage = "atomically publish BMP";
        if (!MoveFileExW(temporary.c_str(), (s_pluginDir + L"RepublicMP.qa.frame.bmp").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) hr = IoError();
    }
    if (FAILED(hr)) {
        DeleteFileW(temporary.c_str());
        ReportFailure(hr, stage);
        return;
    }
    DeleteFileW((s_pluginDir + L"RepublicMP.qa.screenshot.error").c_str());
    if (!DeleteFileW(request.c_str())) { ReportFailure(IoError(), "consume completed screenshot request"); return; }
    Logf("republicmp  QA FRAME: saved game backbuffer to RepublicMP.qa.frame.bmp");
}
