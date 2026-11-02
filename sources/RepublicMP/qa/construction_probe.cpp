// SPDX-License-Identifier: GPL-3.0-only
// Calls only the same LS_CMD_PLACE/DEMOLISH submission path as native tools.
#include "construction_probe.h"
#include "../world/buildings.h"
#include "../world/demolish_hook.h"
#include "../world/terrain_hook.h"
#include "../game/world.h"
#include "../session/save_sync.h"
#include "../determinism/determinism.h"
#include "../determinism/state_hash.h"
#include "../lockstep/commands.h"
#include "../lockstep/lockstep_net.h"
#include <cmath>
#include <string>

namespace {
struct Slot {
    bool chosen, buildQueued, demolishQueued, seenBuilding;
    float pos[3];
};
Slot s_slots[2] = {};
unsigned s_world = ~0u;
std::wstring s_gameDir, s_pluginDir;
const char* s_error = "";
const char kType[] = "monument_emblem";

std::wstring Directory(HMODULE module)
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(module, path, _countof(path));
    if (!n || n >= _countof(path)) return {};
    std::wstring p(path, n);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}
bool Exists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool OptedIn()
{
    return !s_gameDir.empty() && !s_pluginDir.empty() &&
        Exists(s_gameDir + L"TEST-COPY.txt") && Exists(s_pluginDir + L"RepublicMP.qa.enabled");
}
bool Fail(const char* why) { s_error = why; return false; }
bool Ready()
{
    if (!OptedIn()) return Fail("construction probe requires isolated-copy QA markers");
    SessionState state = SaveSync_GetState();
    bool paused = false; int speed = 0;
    Determinism_SessionSpeed(&paused, &speed);
    if (!World_IsReady() || !Determinism_SessionRunning() || !Commands_Capturing() ||
        !LockstepNet_IsConnected() || !paused ||
        (state != SESSION_HOST_IN_GAME && state != SESSION_CLIENT_IN_GAME))
        return Fail("construction probe requires loaded paused connected lockstep");
    return true;
}
bool ReadObject(const unsigned char* building, float pos[3], int* index, int* category, float* progress)
{
    if (!building || !World_BuildingPos(building, pos)) return false;
    __try {
        *index = *(const int*)building;
        *progress = *(const float*)(building + GAME_BUILDING_PROGRESS_OFFSET);
        *category = World_BuildingCategory(building);
        return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) && std::isfinite(*progress);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadFlags(const unsigned char* b, QaProbeBuildingStatus* out)
{
    __try {
        out->payRub = b[0x608]; out->payUsd = b[0x609];
        out->stopped = b[0x879]; out->demolishing = b[0xEA8];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Bounds(unsigned char*** begin, unsigned char*** end)
{
    if (!World_Buildings(begin, end) || !*begin || !*end) return false;
    uintptr_t b = reinterpret_cast<uintptr_t>(*begin), e = reinterpret_cast<uintptr_t>(*end);
    return e >= b && (e - b) % sizeof(void*) == 0 && (e - b) / sizeof(void*) <= 200000;
}
bool Empty(float x, float z, unsigned char** begin, unsigned char** end)
{
    for (auto it = begin; it != end; ++it) {
        float p[3], progress; int index, category;
        if (!*it) continue;
        if (!ReadObject(*it, p, &index, &category, &progress)) return false;
        float dx = p[0] - x, dz = p[2] - z;
        if (dx * dx + dz * dz < 200.0f * 200.0f) return false;
    }
    return true;
}
bool FlatDry(float x, float z, float* height)
{
    float lo = 1.0e30f, hi = -1.0e30f, center = 0;
    for (int iz = -1; iz <= 1; ++iz) for (int ix = -1; ix <= 1; ++ix) {
        float pos[3] = { x + ix * 20.0f, 0.0f, z + iz * 20.0f }, h = 0;
        if (!TerrainHook_HeightAt(pos, &h) || !std::isfinite(h) || h < 2.0f || h > 10000.0f) return false;
        if (h < lo) lo = h;
        if (h > hi) hi = h;
        if (!ix && !iz) center = h;
    }
    if (hi - lo > 0.75f) return false;
    *height = center;
    return true;
}
bool Prime()
{
    unsigned world = Determinism_WorldLoads();
    if (s_world != world) { memset(s_slots, 0, sizeof(s_slots)); s_world = world; }
    if (s_slots[0].chosen && s_slots[1].chosen) return true;
    if (!Exists(s_gameDir + L"media_soviet\\buildings_types\\monument_emblem.ini"))
        return Fail("built-in monument_emblem type unavailable");
    unsigned char** begin = nullptr; unsigned char** end = nullptr;
    if (!Bounds(&begin, &end)) return Fail("building list unreadable");
    // Fixed deterministic candidate order; slots occupy disjoint halves.
    // No camera, cursor, RNG, frame count or local player state is sampled.
    Slot choices[2] = {};
    for (int slot = 0; slot < 2; ++slot) {
        for (int iz = 0; iz < 27 && !choices[slot].chosen; ++iz) {
            float z = -6500.0f + iz * 500.0f;
            for (int ix = 0; ix < 9; ++ix) {
                float x = (slot ? 2500.0f : -6500.0f) + ix * 500.0f, y;
                if (!FlatDry(x, z, &y) || !Empty(x, z, begin, end)) continue;
                choices[slot].chosen = true;
                choices[slot].pos[0] = x; choices[slot].pos[1] = y; choices[slot].pos[2] = z;
                break;
            }
        }
        if (!choices[slot].chosen) return Fail("no bounded flat dry empty construction candidate");
    }
    memcpy(s_slots, choices, sizeof(s_slots));
    Logf("republicmp  QA BUILD: primed world %u targets (%.2f,%.2f,%.2f), (%.2f,%.2f,%.2f)", world,
        choices[0].pos[0], choices[0].pos[1], choices[0].pos[2], choices[1].pos[0], choices[1].pos[1], choices[1].pos[2]);
    return true;
}
}

void QaConstructionProbe_Bind(const TsmHost* host)
{
    TsmBind(host);
    HMODULE own = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&QaConstructionProbe_Bind), &own)) s_pluginDir = Directory(own);
    s_gameDir = Directory(nullptr);
}
const char* QaConstructionProbe_LastError() { return s_error; }

bool GetProbeBuildingStatus(int slot, QaProbeBuildingStatus* out)
{
    s_error = "";
    if (!out || slot < 0 || slot > 1) return Fail("construction slot must be 0 or 1");
    memset(out, 0, sizeof(*out));
    out->slot = slot; out->index = out->category = -1;
    out->optedIn = OptedIn(); out->worldReady = World_IsReady();
    if (!Ready() || !Prime()) return false;
    Slot& target = s_slots[slot];
    out->worldLoads = s_world; out->slotChosen = target.chosen;
    out->buildQueued = target.buildQueued; out->demolishQueued = target.demolishQueued;
    out->x = target.pos[0]; out->y = target.pos[1]; out->z = target.pos[2];
    StateSummary state = {};
    if (!StateHash_Compute(&state)) return Fail("construction state summary unreadable");
    out->totalBuildings = state.buildings; out->rubles = state.rub; out->dollars = state.usd;
    out->hashBuildings = state.hashBuildings;
    BuildMode local = BuildMode_Read(); out->localRub = local.rub; out->localUsd = local.usd;
    unsigned char** begin = nullptr; unsigned char** end = nullptr;
    if (!Bounds(&begin, &end)) return Fail("construction object list unreadable");
    float closest = 30.0f * 30.0f;
    for (auto it = begin; it != end; ++it) {
        if (!*it) continue;
        float pos[3], progress; int index, category;
        if (!ReadObject(*it, pos, &index, &category, &progress)) return Fail("construction object unreadable");
        float dx = pos[0] - target.pos[0], dz = pos[2] - target.pos[2], d = dx * dx + dz * dz;
        if (d > 30.0f * 30.0f) continue;
        ++out->nearbyBuildings;
        if (d < closest) {
            closest = d; out->index = index; out->category = category; out->progress = progress;
            if (!ReadFlags(*it, out)) return Fail("construction flags unreadable");
        }
    }
    out->exists = out->nearbyBuildings > 0;
    if (out->exists) target.seenBuilding = true;
    out->demolished = target.seenBuilding && !out->exists;
    return true;
}

bool QueueTestBuilding(int slot)
{
    QaProbeBuildingStatus status;
    if (!GetProbeBuildingStatus(slot, &status)) return false;
    Slot& target = s_slots[slot];
    if (target.buildQueued || status.exists) return Fail("construction slot already queued or occupied");
    if (status.rubles < 100000.0) return Fail("construction test requires >=100000 rubles for a bounded paid fixture");
    BuildingMsg msg = {};
    msg.kind = BUILDING_PLACE;
    msg.mode = {1, 0, 0, 0}; // paid ruble construction; native ticks advance it
    msg.x = msg.cursorX = target.pos[0]; msg.y = msg.cursorY = target.pos[1]; msg.z = msg.cursorZ = target.pos[2];
    strncpy_s(msg.typeName, kType, _TRUNCATE);
    // Native 2BADE0 does not read its fifth stack argument in this pinned
    // build; stock tool call sites also leave it unset. Use deterministic 0.
    msg.param5 = 0; msg.cursorRot = 0; msg.mirrored = 0; msg.pickModes = 0;
    Commands_Submit(LS_CMD_PLACE, &msg, sizeof(msg));
    target.buildQueued = true;
    return true; // queued, NOT a successful construction assertion
}

bool QueueTestDemolish(int slot)
{
    QaProbeBuildingStatus status;
    if (!GetProbeBuildingStatus(slot, &status)) return false;
    Slot& target = s_slots[slot];
    if (target.demolishQueued || status.nearbyBuildings != 1 || status.index < 0 || status.category < 0)
        return Fail("demolition requires exactly one observed slot building and no previous demolition");
    // Use the object's exact position: native replay lookup is sub-meter.
    unsigned char** begin = nullptr; unsigned char** end = nullptr;
    if (!Bounds(&begin, &end)) return Fail("demolition building list unreadable");
    BuildingMsg msg = {}; bool found = false; unsigned char* nativeTarget = nullptr;
    for (auto it = begin; it != end; ++it) {
        if (!*it) continue;
        float pos[3], progress; int index, category;
        if (!ReadObject(*it, pos, &index, &category, &progress)) return Fail("demolition target unreadable");
        if (index != status.index || category != status.category) continue;
        msg.x = pos[0]; msg.y = pos[1]; msg.z = pos[2]; nativeTarget = *it; found = true; break;
    }
    if (!found) return Fail("demolition target disappeared before submission");
    msg.kind = BUILDING_DEMOLISH; msg.demolishCategory = status.category;
    msg.mode = {1, 0, 0, 0}; // same paid mode the native demolition tool reads
    // Stock fast terrain_demolish path RVA 3AD424..3AD442: r8b=1,r9b=1,
    // stack arg5=0,arg7=1. Arg6 is scratch/overwritten in native 168730.
    msg.demolishArgs[0] = 1; msg.demolishArgs[1] = 1; msg.demolishArgs[2] = 0;
    msg.demolishArgs[3] = 0; msg.demolishArgs[4] = 1;
    // Exercise the real capture hook, not a manually populated wire message.
    // This is the native bulldozer's pinned entry/ABI, called while paused on
    // the main thread; the hook queues the action for both peers.
    if (!Commands_TakeLocal() || g_exeBase[HOOK_DEMOLISH_RVA] == 0x48)
        return Fail("native demolition capture hook unavailable at this boundary");
    using NativeDemolish = void (*)(__int64, __int64, unsigned long long, unsigned long long,
        unsigned long long, unsigned long long, unsigned long long);
    BuildMode mine = BuildMode_Read(); bool captured = true;
    __try {
        __try {
            BuildMode_Write(msg.mode);
            reinterpret_cast<NativeDemolish>(g_exeBase + HOOK_DEMOLISH_RVA)(
                reinterpret_cast<__int64>(g_exeBase + BUILD_MODE_THIS_RVA), reinterpret_cast<__int64>(nativeTarget), 1, 1, 0, 0, 1);
        } __finally { BuildMode_Write(mine); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { captured = false; }
    if (!captured) return Fail("native demolition capture threw");
    target.demolishQueued = true;
    return true;
}
