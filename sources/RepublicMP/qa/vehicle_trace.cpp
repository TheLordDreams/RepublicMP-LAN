// SPDX-License-Identifier: GPL-3.0-only
// Observe native vehicle updates; never skip, reorder or replace their effects.
#include "vehicle_trace.h"
#include "../determinism/determinism.h"
#include <intrin.h>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <limits>
#include <locale>

namespace {
// Validated for the pinned executable: local/crash-range.asm, 66D530..66D542.
constexpr unsigned kUpdateRva = 0x66D530;
constexpr unsigned kIndexOffset = 4;     // state_hash.cpp VEH_INDEX_OFFSET
constexpr unsigned kPositionOffset = 0xDE0; // state_hash.cpp VEH_POS_OFFSET
constexpr unsigned kTimerRva = 0x9D4EE0; // determinism.cpp GAME_TIMER_RVA
constexpr unsigned kMaxRecords = 8192;
constexpr unsigned char kPrologue[19] = {
    0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x55,0x53,0x41,
    0x54,0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0xA1
};
// Win64 RCX=this. Preserve the caller's RDX as well: worker2594DA leaves
// queue index there, main3FADE2 does not define it as an argument. The native
// entry saves only RCX; before its first call it defines R8/R9 and XMM1 and
// never reads/saves incoming RDX. Native epilogue66ED86..66EDCD establishes
// no return value; both observed callers ignore the return registers.
using UpdateFn = void (*)(void*, unsigned long long);
UpdateFn s_original = nullptr;
std::wstring s_pluginDir, s_gameDir, s_file;
volatile LONG s_enabled = 0, s_sequence = 0;
bool s_installed = false, s_created = false, s_failed = false;
struct Snapshot {
    int index;
    float pos[3];
    unsigned char timer[16];
};
struct Record {
    unsigned sequence, world, caller, thread;
    unsigned long long tick;
    bool isStepFrame, inStepFrame, beforeValid, afterValid;
    Snapshot before, after;
};
Record s_records[kMaxRecords] = {};
unsigned s_count = 0, s_flushed = 0;

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
bool OptedIn()
{
    return !s_pluginDir.empty() && !s_gameDir.empty() &&
        Marker(s_gameDir + L"TEST-COPY.txt") &&
        Marker(s_pluginDir + L"RepublicMP.qa.enabled") &&
        Marker(s_pluginDir + L"RepublicMP.qa.vehicle-trace.enabled");
}
bool ReadSnapshot(void* vehicle, Snapshot* snapshot)
{
    __try {
        auto* p = static_cast<unsigned char*>(vehicle);
        snapshot->index = *reinterpret_cast<int*>(p + kIndexOffset);
        memcpy(snapshot->pos, p + kPositionOffset, sizeof(snapshot->pos));
        memcpy(snapshot->timer, g_exeBase + kTimerRva, sizeof(snapshot->timer));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static __declspec(noinline) void Hook(void* vehicle, unsigned long long callerIndex)
{
    bool trace = InterlockedCompareExchange(&s_enabled, 0, 0) &&
        Determinism_IsActive() && Determinism_SessionRunning();
    unsigned long long tick = trace ? Determinism_StepCount() : 0;
    if (!trace || tick > 12) { s_original(vehicle, callerIndex); return; }
    LONG sequence = InterlockedIncrement(&s_sequence);
    if (sequence > static_cast<LONG>(kMaxRecords)) { s_original(vehicle, callerIndex); return; }
    Record record = {};
    record.sequence = static_cast<unsigned>(sequence);
    record.world = Determinism_WorldLoads();
    record.tick = tick;
    record.thread = GetCurrentThreadId();
    BYTE* caller = static_cast<BYTE*>(_ReturnAddress());
    if (caller >= g_exeBase && caller < g_exeBase + g_exeSize)
        record.caller = static_cast<unsigned>(caller - g_exeBase);
    record.isStepFrame = Determinism_IsStepFrame();
    record.inStepFrame = Determinism_InStepFrame();
    record.beforeValid = ReadSnapshot(vehicle, &record.before);
    // Do not hold any trace lock or catch native exceptions across the game.
    s_original(vehicle, callerIndex);
    record.afterValid = ReadSnapshot(vehicle, &record.after);
    EnterCriticalSection(&g_lock);
    if (s_count < kMaxRecords) s_records[s_count++] = record;
    LeaveCriticalSection(&g_lock);
}
void SnapshotJson(std::ostringstream& out, bool valid, const Snapshot& snapshot)
{
    if (!valid) { out << "null"; return; }
    out << "{\"index\":" << snapshot.index << ",\"pos\":[";
    for (unsigned i = 0; i < 3; ++i) {
        if (i) out << ',';
        if (std::isfinite(snapshot.pos[i])) out << snapshot.pos[i]; else out << "null";
    }
    out << "],\"timer16\":\"";
    static const char digits[] = "0123456789abcdef";
    for (unsigned char b : snapshot.timer) out << digits[b >> 4] << digits[b & 15];
    out << "\"}";
}
}

void QaVehicleTrace_Bind(const TsmHost* host)
{
    TsmBind(host);
    HMODULE own = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&QaVehicleTrace_Bind), &own)) s_pluginDir = Directory(own);
    s_gameDir = Directory(g_exe);
    if (!OptedIn()) return;
    s_file = s_pluginDir + L"RepublicMP.qa.vehicle-trace." + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    s_installed = InstallInlineHook(g_exeBase + kUpdateRva, reinterpret_cast<void*>(&Hook),
        reinterpret_cast<void**>(&s_original), kPrologue, sizeof(kPrologue), "QA observe vehicle update 66D530");
    InterlockedExchange(&s_enabled, s_installed ? 1 : 0);
    Logf("republicmp QA VEHICLE TRACE: hook installed=%d; diagnostic only, native calls unchanged; max %u records at completed ticks 0..12",
         s_installed, kMaxRecords);
}

void QaVehicleTrace_Flush()
{
    if (!s_installed || s_failed) return;
    bool enabled = OptedIn();
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
    if (!enabled) return;
    Record batch[256];
    EnterCriticalSection(&g_lock);
    unsigned n = s_count - s_flushed;
    if (n > _countof(batch)) n = _countof(batch);
    for (unsigned i = 0; i < n; ++i) batch[i] = s_records[s_flushed + i];
    LeaveCriticalSection(&g_lock);
    if (!n) return;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    if (!s_created) out << "{\"schema\":1,\"kind\":\"vehicle-trace\",\"pid\":" << GetCurrentProcessId()
        << ",\"maxRecords\":" << kMaxRecords << ",\"lastCompletedTick\":12}\n";
    for (unsigned i = 0; i < n; ++i) {
        const Record& r = batch[i];
        out << "{\"sequence\":" << r.sequence << ",\"worldLoad\":" << r.world
            << ",\"completedTick\":" << r.tick << ",\"thread\":" << r.thread
            << ",\"callerRva\":\"0x" << std::hex << r.caller << std::dec << "\""
            << ",\"isStepFrame\":" << (r.isStepFrame ? "true" : "false")
            << ",\"inStepFrame\":" << (r.inStepFrame ? "true" : "false") << ",\"before\":";
        SnapshotJson(out, r.beforeValid, r.before);
        out << ",\"after\":";
        SnapshotJson(out, r.afterValid, r.after);
        out << "}\n";
    }
    std::string data = out.str();
    HANDLE file = CreateFileW(s_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, s_created ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        s_failed = true;
        InterlockedExchange(&s_enabled, 0);
        Logf("republicmp QA VEHICLE TRACE: output could not be created/opened, error %lu", GetLastError());
        return;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) {
        s_failed = true;
        InterlockedExchange(&s_enabled, 0);
        Logf("republicmp QA VEHICLE TRACE: output failed; trace disabled, last line may be incomplete");
        return;
    }
    s_created = true;
    s_flushed += n;
}
