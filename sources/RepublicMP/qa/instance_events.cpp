// SPDX-License-Identifier: GPL-3.0-only
// Independent native workers for two disposable clients on the same Windows
// session. This changes event names only, never simulation routines or timing.
#include "instance_events.h"
#include <string>
#include <wchar.h>

namespace {
using CreateEventFn = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
CreateEventFn s_original = nullptr;
bool s_bound = false;
volatile LONG s_seen[3] = {};
const wchar_t* const kNames[] = { L"FillingrStart", L"RenderStart", L"RenderFinished" };
const char* const kLabels[] = { "FillingrStart", "RenderStart", "RenderFinished" };

// Static evidence for the exact executable allowed by core/build_guard.h:
// 2592DD loads UTF-16 "FillingrStart" from RVA8916C0;2592EA CreateEventW
// uses manual-reset=1, initial=0 and stores HANDLE at worker+60.
// 47D220 loads "RenderStart" from8D3EF8;47D22D creates/stores renderer+60.
// 47D238 loads "RenderFinished" from8D3FC0;47D248 stores renderer+68.
// All three calls use EXE KERNEL32!CreateEventW IAT86D820. EXE and engine
// import no OpenEventA/W; native users signal/wait through stored handles.
// Engine has CreateEventW IAT16B060, but none of these proven constructors
// use it. Leave that table and every unknown event name untouched.
std::wstring Directory(HMODULE module)
{
    wchar_t file[32768];
    DWORD n = GetModuleFileNameW(module, file, _countof(file));
    if (!n || n >= _countof(file)) return {};
    std::wstring p(file, n);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}
bool Marker(const std::wstring& file)
{
    DWORD attr = GetFileAttributesW(file.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
int KnownName(LPCWSTR name)
{
    if (!name) return -1;
    __try {
        for (int i = 0; i < _countof(kNames); ++i) if (wcscmp(name, kNames[i]) == 0) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return -1;
}
HANDLE WINAPI HookCreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    int which = KnownName(name);
    if (which < 0) return s_original(attributes, manualReset, initialState, name);
    // Keep name reuse within THIS process while separating both test clients.
    // Fixed stack storage: no allocation or mutation of the game's string.
    wchar_t isolated[128];
    int n = swprintf_s(isolated, _countof(isolated), L"Local\\RepublicMP.QA.%lu.%ls", GetCurrentProcessId(), kNames[which]);
    if (n < 0) return s_original(attributes, manualReset, initialState, name);
    HANDLE result = s_original(attributes, manualReset, initialState, isolated);
    DWORD error = GetLastError();
    if (InterlockedCompareExchange(&s_seen[which], 1, 0) == 0)
        Logf("republicmp QA INSTANCE EVENTS: %s isolated by PID; created=%d; error=%lu", kLabels[which], result != nullptr, error);
    SetLastError(error); // Preserve CreateEventW's ERROR_ALREADY_EXISTS contract.
    return result;
}
bool EventsAlreadyCreated()
{
    // Refuse late installation rather than leave some handles shared and some
    // private. These offsets are the native fields documented above and used
    // by determinism.cpp / sim_patches.cpp, not new gameplay addresses.
    __try {
        BYTE* filling = *reinterpret_cast<BYTE**>(g_exeBase + 0x995290);
        BYTE* renderer = *reinterpret_cast<BYTE**>(g_exeBase + 0x995268);
        return (filling && *reinterpret_cast<HANDLE*>(filling + 0x60)) ||
            (renderer && (*reinterpret_cast<HANDLE*>(renderer + 0x60) ||
                          *reinterpret_cast<HANDLE*>(renderer + 0x68)));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
}

void QaInstanceEvents_Bind(const TsmHost* host)
{
    if (s_bound) return;
    s_bound = true;
    TsmBind(host);
    HMODULE own = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&QaInstanceEvents_Bind), &own)) return;
    std::wstring pluginDir = Directory(own), gameDir = Directory(g_exe);
    if (pluginDir.empty() || gameDir.empty() || !Marker(gameDir + L"TEST-COPY.txt") ||
        !Marker(pluginDir + L"RepublicMP.qa.enabled") ||
        !Marker(pluginDir + L"RepublicMP.qa.isolate-events.enabled")) return;
    if (EventsAlreadyCreated()) {
        Logf("republicmp QA INSTANCE EVENTS: REFUSED late/unknown native worker initialization; restart isolated copy with markers present before injection");
        return;
    }
    bool installed = PatchIat(g_exe, "KERNEL32.dll", "CreateEventW", reinterpret_cast<void*>(&HookCreateEventW),
        reinterpret_cast<void**>(&s_original), "QA isolate native worker events");
    Logf("republicmp QA INSTANCE EVENTS: CreateEventW isolation installed=%d; only FillingrStart/RenderStart/RenderFinished; physics unchanged", installed);
    // Once native handles are created their namespace must remain stable until
    // process exit. Marker removal cannot safely undo or switch this IAT hook.
}
