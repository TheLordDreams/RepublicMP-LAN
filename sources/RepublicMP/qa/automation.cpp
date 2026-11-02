// SPDX-License-Identifier: GPL-3.0-only
// Local file mailbox for isolated, opt-in two-client QA. No socket server.
#include "automation.h"
#include "construction_probe.h"
#include "../game/save_load.h"
#include "../game/world.h"
#include "../game/main_menu_hook.h"
#include "../session/save_sync.h"
#include "../session/game_speed.h"
#include "../determinism/determinism.h"
#include "../determinism/state_hash.h"
#include "../lockstep/lockstep_net.h"
#include "../lockstep/commands.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <cerrno>
#include <cmath>
#include <limits>
#include <locale>
#include <deque>
#include <utility>
#include <cstdio>

namespace {
std::wstring s_pluginDir, s_gameDir;
volatile LONG s_enabled = 0;
ULONGLONG s_nextPoll = 0, s_nextStatus = 0, s_sequence = 0;
std::string s_error;
struct Sample {
    unsigned world;
    ULONGLONG tick;
    StateSummary state;
    bool vehicleRecordsValid;
    std::vector<VehicleRecord> vehicleRecords;
};
std::deque<Sample> s_samples;
constexpr size_t kMaxSamples = 128;
unsigned s_sampleWorld = 0;
bool s_proxyLockstep = false;
struct PausedSample {
    bool valid;
    unsigned world;
    ULONGLONG tick, epoch;
    StateSummary state;
};
PausedSample s_paused = {};
enum ProbeVerb { PROBE_NONE, PROBE_PRIME, PROBE_BUILD, PROBE_DEMOLISH };
ProbeVerb s_probePending = PROBE_NONE;
int s_probeSlot = 0;
bool s_probePrimed = false;
struct ProbeSnapshot { bool pending, valid; char error[192]; QaProbeBuildingStatus slots[2]; };
ProbeSnapshot s_probe = {};

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t file[32768];
    DWORD n = GetModuleFileNameW(module, file, _countof(file));
    if (!n || n >= _countof(file)) return {};
    std::wstring p(file, n);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}
bool FileExists(const std::wstring& name)
{
    DWORD a = GetFileAttributesW(name.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool OptedIn()
{
    return !s_pluginDir.empty() && !s_gameDir.empty() &&
        FileExists(s_pluginDir + L"RepublicMP.qa.enabled") &&
        FileExists(s_gameDir + L"TEST-COPY.txt");
}
std::string Json(const char* text)
{
    std::ostringstream out;
    out << '"';
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p == '"' || *p == '\\') out << '\\' << (char)*p;
        else if (*p < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << (unsigned)*p << std::dec;
        else out << (char)*p;
    }
    out << '"';
    return out.str();
}
std::string Hex(ULONGLONG value)
{
    std::ostringstream out;
    out << '"' << std::hex << std::setw(16) << std::setfill('0') << value << '"';
    return out.str();
}
// Private, opt-in diagnostic snapshots; never part of a normal mod session.
// Pointers remain process-local and are intentionally NOT included in hashes.
void CaptureVehicleBytes(ULONGLONG tick, const wchar_t* phase = L"post")
{
    if (tick > 4 || !FileExists(s_pluginDir + L"RepublicMP.qa.vehicle-bytes.enabled")) return;
    BYTE** bounds[2] = {};
    SIZE_T done = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), g_exeBase + 0x9E7720, bounds, sizeof(bounds), &done) || done != sizeof(bounds)) return;
    uintptr_t begin = (uintptr_t)bounds[0], end = (uintptr_t)bounds[1];
    if (end < begin || (end - begin) % sizeof(void*) || (end - begin) / sizeof(void*) > 65536) return;
    std::vector<BYTE*> vehicles((end - begin) / sizeof(void*));
    if (!vehicles.empty() && (!ReadProcessMemory(GetCurrentProcess(), bounds[0], vehicles.data(), vehicles.size() * sizeof(void*), &done) || done != vehicles.size() * sizeof(void*))) return;
    std::wstring file = s_pluginDir + L"RepublicMP.qa.vehicles." + phase + L"." + std::to_wstring(tick) + L".bin";
    FILE* out = nullptr;
    if (_wfopen_s(&out, file.c_str(), L"wb") || !out) return;
    unsigned count = (unsigned)vehicles.size(), bytes = 0x1AD8;
    fwrite(&count, sizeof(count), 1, out); fwrite(&bytes, sizeof(bytes), 1, out);
    std::vector<BYTE> record(bytes);
    for (BYTE* vehicle : vehicles) {
        if (!vehicle || !ReadProcessMemory(GetCurrentProcess(), vehicle, record.data(), bytes, &done) || done != bytes) break;
        fwrite(record.data(), 1, bytes, out);
    }
    fclose(out);
}
void CaptureSample(ULONGLONG tick)
{
    Sample sample = {};
    sample.tick = tick;
    sample.world = Determinism_WorldLoads();
    if (!StateHash_Compute(&sample.state)) return;
    CaptureVehicleBytes(tick);
    sample.vehicleRecordsValid = StateHash_VehicleRecords(sample.vehicleRecords);
    if (!sample.vehicleRecordsValid) sample.vehicleRecords.clear();
    EnterCriticalSection(&g_lock);
    if (sample.world != s_sampleWorld) { s_samples.clear(); s_sampleWorld = sample.world; }
    if (s_samples.size() == kMaxSamples) s_samples.pop_front();
    s_samples.push_back(std::move(sample));
    LeaveCriticalSection(&g_lock);
}
// Main simulation thread, after a completed Early+Late tick. No asynchronous
// Present reads are used for a running simulation's record or hash.
void OnStep()
{
    if (!InterlockedCompareExchange(&s_enabled, 0, 0) || !Determinism_SessionRunning()) return;
    ULONGLONG tick = Determinism_StepCount();
    if (!tick || (tick > 120 && tick % LOCKSTEP_HASH_TICKS)) return;
    CaptureSample(tick);
}
void OnFrame()
{
    if (!InterlockedCompareExchange(&s_enabled, 0, 0) || !Determinism_SessionRunning()) return;
    ProbeVerb verb; int slot;
    EnterCriticalSection(&g_lock); verb = s_probePending; slot = s_probeSlot; LeaveCriticalSection(&g_lock);
    if (verb != PROBE_NONE || s_probePrimed) {
        ProbeSnapshot probe = {};
        if (verb == PROBE_NONE) { EnterCriticalSection(&g_lock); memcpy(probe.error, s_probe.error, sizeof(probe.error)); LeaveCriticalSection(&g_lock); }
        bool ok = true;
        if (verb == PROBE_PRIME) {
            ok = GetProbeBuildingStatus(0, &probe.slots[0]) && GetProbeBuildingStatus(1, &probe.slots[1]);
            s_probePrimed = ok;
        } else if (verb == PROBE_BUILD) ok = QueueTestBuilding(slot);
        else if (verb == PROBE_DEMOLISH) ok = QueueTestDemolish(slot);
        if (!ok) strncpy_s(probe.error, QaConstructionProbe_LastError(), _TRUNCATE);
        if (ok && s_probePrimed) {
            bool paused; int speed; Determinism_SessionSpeed(&paused, &speed);
            if (paused) {
                probe.valid = GetProbeBuildingStatus(0, &probe.slots[0]) && GetProbeBuildingStatus(1, &probe.slots[1]);
                if (!probe.valid) strncpy_s(probe.error, QaConstructionProbe_LastError(), _TRUNCATE);
            }
        }
        EnterCriticalSection(&g_lock);
        s_probe = probe; s_probePending = PROBE_NONE;
        LeaveCriticalSection(&g_lock);
    }
    PausedSample sample = {};
    bool paused; int speed;
    Determinism_SessionSpeed(&paused, &speed);
    if (paused && World_IsReady()) {
        sample.world = Determinism_WorldLoads();
        sample.tick = Determinism_StepCount();
        sample.epoch = Commands_ApplyEpoch();
        sample.valid = StateHash_Compute(&sample.state);
    }
    EnterCriticalSection(&g_lock); s_paused = sample; LeaveCriticalSection(&g_lock);
    if (Determinism_IsStepFrame()) CaptureVehicleBytes(Determinism_StepCount(), L"pre");
}
// Tick zero is the one exception: after both sides joined, the shared world
// is stopped and no step has ever run. Capture before polling a resume command.
void CaptureJoinedBaseline()
{
    SessionState state = SaveSync_GetState();
    if ((state != SESSION_HOST_IN_GAME && state != SESSION_CLIENT_IN_GAME) ||
        !Determinism_SessionRunning() || !World_IsReady() || Determinism_StepCount() != 0 ||
        !LockstepNet_IsConnected()) return;
    bool paused; int speed;
    Determinism_SessionSpeed(&paused, &speed);
    if (!paused) return;
    unsigned world = Determinism_WorldLoads();
    EnterCriticalSection(&g_lock);
    bool empty = s_samples.empty() || s_sampleWorld != world;
    LeaveCriticalSection(&g_lock);
    if (empty) CaptureSample(0);
}
bool SafeSaveName(const std::string& name)
{
    if (name.size() <= 7 || name.size() > 90 || name.compare(0, 7, "coopqa_") != 0) return false;
    for (unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
void Execute(const std::string& verb, const std::string& arg)
{
    SessionState state = SaveSync_GetState();
    if (verb == "probe-prime" || verb == "build-test" || verb == "demolish-test") {
        bool paused; int speed; Determinism_SessionSpeed(&paused, &speed);
        if (!Determinism_SessionRunning() || !paused || !World_IsReady()) { s_error = "probe requires paused running world"; return; }
        if ((verb == "probe-prime" && !arg.empty()) || (verb != "probe-prime" && arg != "0" && arg != "1")) { s_error = "invalid construction probe argument"; return; }
        EnterCriticalSection(&g_lock);
        if (s_probe.pending) { LeaveCriticalSection(&g_lock); s_error = "construction probe busy"; return; }
        s_probeSlot = arg == "1" ? 1 : 0;
        s_probePending = verb == "probe-prime" ? PROBE_PRIME : verb == "build-test" ? PROBE_BUILD : PROBE_DEMOLISH;
        s_probe.pending = true; s_probe.error[0] = 0;
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (verb == "state" && arg.empty()) return;
    if (verb == "disconnect" && arg.empty()) { SaveSync_RequestDisconnect(); return; }
    if (verb == "host" && arg.empty()) {
        if (state != SESSION_IDLE || !World_IsReady()) { s_error = "host requires idle loaded world"; return; }
        SaveSync_StartHost(SESSION_MODE_COOP); return;
    }
    if (verb == "join") {
        if (arg != "127.0.0.1") { s_error = "join allows only 127.0.0.1"; return; }
        if (state != SESSION_IDLE) { s_error = "join requires idle session"; return; }
        SaveSync_StartClient("127.0.0.1"); return;
    }
    if (verb == "load") {
        if (state != SESSION_IDLE || !SafeSaveName(arg)) { s_error = "load requires idle and safe coopqa_ save name"; return; }
        if (MainMenuHook_BuildCount() < 2 && !World_IsReady()) { s_error = "root menu has not initialized"; return; }
        std::wstring name(arg.begin(), arg.end());
        DWORD a = GetFileAttributesW((s_gameDir + L"media_soviet\\save\\" + name).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { s_error = "QA save directory does not exist"; return; }
        TriggerLoadSave(arg.c_str()); return;
    }
    if (verb == "credit-test" || verb == "save") {
        bool paused; int speed;
        Determinism_SessionSpeed(&paused, &speed);
        if ((state != SESSION_HOST_IN_GAME && state != SESSION_CLIENT_IN_GAME) ||
            !Determinism_SessionRunning() || !World_IsReady() || !paused || !Commands_Capturing()) {
            s_error = "credit-test/save requires loaded paused running lockstep session"; return;
        }
        if (verb == "credit-test") {
            if (!arg.empty()) { s_error = "credit-test accepts no argument"; return; }
            const LsMoneyDelta delta = { 100.0, 0.0 };
            Commands_Submit(LS_CMD_MONEY, &delta, sizeof(delta));
        } else {
            if (!SafeSaveName(arg)) { s_error = "save requires safe coopqa_ name"; return; }
            // Same request as the save UI. The next hooked Late intercepts it
            // in SharedSave_BeforeLate before native save I/O, queues LS_CMD_SAVE,
            // and creates the save directory on both PCs when that command applies.
            TriggerSaveGame(arg.c_str());
        }
        return;
    }
    if ((verb == "pause" || verb == "resume") && arg.empty()) {
        if (state != SESSION_HOST_IN_GAME && state != SESSION_CLIENT_IN_GAME) { s_error = "pause/resume requires running lockstep session"; return; }
        bool paused; int speed;
        Determinism_SessionSpeed(&paused, &speed);
        if (paused != (verb == "pause") && !Determinism_RequestPauseToggle()) s_error = "pause request refused";
        return;
    }
    s_error = "unknown command or invalid argument";
}
void Poll()
{
    HANDLE f = CreateFileW((s_pluginDir + L"RepublicMP.qa.command").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char bytes[1025]; DWORD n = 0;
    bool ok = ReadFile(f, bytes, sizeof(bytes), &n, nullptr) != FALSE;
    CloseHandle(f);
    if (!ok || !n || n > 1024) return;
    std::string command(bytes, n);
    while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) command.pop_back();
    auto a = command.find('|'), b = a == std::string::npos ? a : command.find('|', a + 1);
    if (a == std::string::npos || b == std::string::npos || command.find('|', b + 1) != std::string::npos) return;
    std::string seq = command.substr(0, a);
    if (seq.empty() || seq.find_first_not_of("0123456789") != std::string::npos) return;
    errno = 0; char* end = nullptr;
    ULONGLONG sequence = _strtoui64(seq.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || sequence <= s_sequence) return;
    s_sequence = sequence;
    s_error.clear();
    Execute(command.substr(a + 1, b - a - 1), command.substr(b + 1));
    s_nextStatus = 0; // acknowledge accepted/refused requests immediately
}
void Status()
{
    bool paused = true; int speed = 1;
    Determinism_SessionSpeed(&paused, &speed);
    LockstepPlayer players[PROFILE_MAX_PLAYERS];
    int count = LockstepNet_Players(players, _countof(players));
    unsigned world = Determinism_WorldLoads();
    bool loaded = World_IsReady();
    PausedSample pausedSample;
    ProbeSnapshot probe;
    EnterCriticalSection(&g_lock); pausedSample = s_paused; probe = s_probe; LeaveCriticalSection(&g_lock);
    double rubles = std::numeric_limits<double>::quiet_NaN(), dollars = rubles;
    bool moneySnapshot = loaded && paused && Determinism_SessionRunning() && pausedSample.valid &&
        pausedSample.world == world && pausedSample.tick == Determinism_StepCount();
    if (moneySnapshot) { rubles = pausedSample.state.rub; dollars = pausedSample.state.usd; }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"schema\":1,\"pid\":" << GetCurrentProcessId()
        << ",\"version\":" << Json(SaveSync_GetLocalVersion())
        << ",\"uptimeMs\":" << GetTickCount64() << ",\"lastSequence\":" << s_sequence
        << ",\"commandError\":" << Json(s_error.c_str())
        << ",\"sessionError\":" << Json(SaveSync_GetLastError())
        << ",\"loaded\":" << (loaded ? "true" : "false")
        << ",\"menuBuildCount\":" << MainMenuHook_BuildCount()
        << ",\"sessionState\":" << (int)SaveSync_GetState()
        << ",\"sessionRunning\":" << (Determinism_SessionRunning() ? "true" : "false")
        << ",\"host\":" << (SaveSync_IsHostRole() ? "true" : "false")
        << ",\"paused\":" << (paused ? "true" : "false") << ",\"speed\":" << speed
        << ",\"tick\":" << Determinism_StepCount() << ",\"worldLoad\":" << world
        << ",\"peers\":" << LockstepNet_PeerCount() << ",\"players\":" << count
        << ",\"lastMatchedTick\":" << LockstepNet_LastMatchedTick()
        << ",\"desync\":" << Json(LockstepNet_Desync())
        << ",\"moneyRubles\":";
    if (moneySnapshot && std::isfinite(rubles)) out << rubles; else out << "null";
    out << ",\"moneyDollars\":";
    if (moneySnapshot && std::isfinite(dollars)) out << dollars; else out << "null";
    out << ",\"commandsApplyEpoch\":";
    if (moneySnapshot) out << pausedSample.epoch; else out << "null";
    out << ",\"pausedState\":";
    if (moneySnapshot) {
        const StateSummary& h = pausedSample.state;
        out << "{\"all\":" << Hex(StateHash_All(h)) << ",\"buildingsHash\":" << Hex(h.hashBuildings)
            << ",\"vehiclesHash\":" << Hex(h.hashVehicles) << ",\"citizensHash\":" << Hex(h.hashCitizens)
            << ",\"buildings\":" << h.buildings << ",\"vehicles\":" << h.vehicles << ",\"citizens\":" << h.citizens << '}';
    } else out << "null";
    out << ",\"probeCommandPending\":" << (probe.pending ? "true" : "false") << ",\"probeError\":" << Json(probe.error)
        << ",\"probeValid\":" << (probe.valid ? "true" : "false") << ",\"probeSlots\":[";
    for (int i = 0; i < 2; ++i) {
        const auto& p = probe.slots[i]; if (i) out << ',';
        out << "{\"slot\":" << i << ",\"chosen\":" << (p.slotChosen ? "true" : "false")
            << ",\"exists\":" << (p.exists ? "true" : "false") << ",\"demolished\":" << (p.demolished ? "true" : "false")
            << ",\"nearby\":" << p.nearbyBuildings << ",\"index\":" << p.index << ",\"progress\":" << p.progress
            << ",\"payRub\":" << p.payRub << ",\"payUsd\":" << p.payUsd << ",\"stopped\":" << p.stopped
            << ",\"demolishing\":" << p.demolishing << ",\"localRub\":" << p.localRub << ",\"localUsd\":" << p.localUsd
            << ",\"x\":" << p.x << ",\"y\":" << p.y << ",\"z\":" << p.z << '}';
    }
    out << "],\"samples\":[";
    EnterCriticalSection(&g_lock);
    bool first = true;
    for (const Sample& sample : s_samples) {
        if (sample.world != world) continue;
        if (!first) out << ',';
        first = false;
        const StateSummary& h = sample.state;
        out << "{\"tick\":" << sample.tick << ",\"all\":" << Hex(StateHash_All(h))
            << ",\"clock\":" << Hex(h.hashClock) << ",\"buildingsHash\":" << Hex(h.hashBuildings)
            << ",\"vehiclesHash\":" << Hex(h.hashVehicles) << ",\"citizensHash\":" << Hex(h.hashCitizens)
            << ",\"buildings\":" << h.buildings << ",\"vehicles\":" << h.vehicles
            << ",\"citizens\":" << h.citizens
            << ",\"vehicleRecordsValid\":" << (sample.vehicleRecordsValid ? "true" : "false")
            << ",\"vehicleRecords\":[";
        bool firstVehicle = true;
        for (const VehicleRecord& vehicle : sample.vehicleRecords) {
            if (!firstVehicle) out << ',';
            firstVehicle = false;
            out << "{\"index\":" << vehicle.index << ",\"pos\":[";
            for (int axis = 0; axis < 3; ++axis) {
                if (axis) out << ',';
                if (std::isfinite(vehicle.pos[axis])) out << vehicle.pos[axis]; else out << "null";
            }
            out << "],\"cargo\":" << Hex(vehicle.cargo) << '}';
        }
        out << "]}";
    }
    LeaveCriticalSection(&g_lock);
    out << "]}\n";
    std::string data = out.str();
    std::wstring target = s_pluginDir + L"RepublicMP.qa.status.json", temp = target + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    if (ok) ok = FlushFileBuffers(f) != FALSE;
    CloseHandle(f);
    if (ok) MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}

void QaAutomation_Bind(const TsmHost* host)
{
    TsmBind(host);
    HMODULE own = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&QaAutomation_Bind), &own)) s_pluginDir = ModuleDirectory(own);
    s_gameDir = ModuleDirectory(g_exe);
    s_proxyLockstep = OptedIn() && FileExists(s_pluginDir + L"RepublicMP.qa.proxy.enabled");
    if (s_proxyLockstep) Logf("republicmp QA NETWORK: only loopback lockstep connects redirected 5560 -> 5561 for private TCP proxy");
    Determinism_AddStepObserver(OnStep);
    Determinism_AddFrameObserver(OnFrame);
}
void QaAutomation_Tick()
{
    // Revoke automation on the next frame if either marker is removed.
    bool enabled = OptedIn();
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
    if (!enabled) return;
    CaptureJoinedBaseline();
    ULONGLONG now = GetTickCount64();
    if (now >= s_nextPoll) { s_nextPoll = now + 100; Poll(); }
    if (now >= s_nextStatus) { s_nextStatus = now + 500; Status(); }
}
unsigned short QaAutomation_ConnectPort(const char* hostIp, unsigned short port)
{
    return s_proxyLockstep && hostIp && strcmp(hostIp, "127.0.0.1") == 0 && port == 5560 ? 5561 : port;
}
