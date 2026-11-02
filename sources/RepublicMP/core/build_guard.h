// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "supported_build.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

// Call before any module Bind: some Bind methods install hooks/register callbacks.
// Read disk files as data. Never LoadLibrary an unverified game executable.
static bool BuildGuard_ModuleMatches(HMODULE module, const char* expected, const char* label)
{
    if (!module) return false;
    wchar_t file[32768];
    DWORD count = GetModuleFileNameW(module, file, _countof(file));
    if (!count || count >= _countof(file)) return false;
    HANDLE input = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE bytes[65536], digest[32];
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(input, bytes, sizeof(bytes), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, bytes, read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(input);
    if (!ok) { Logf("republicmp BUILD GUARD: cannot hash %s, refusing initialization", label); return false; }
    char hex[65];
    static const char alphabet[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(digest); ++i) {
        hex[2*i] = alphabet[digest[i] >> 4]; hex[2*i+1] = alphabet[digest[i] & 15];
    }
    hex[64] = 0;
    ok = strcmp(hex, expected) == 0;
    Logf("republicmp BUILD GUARD: %s SHA256=%s %s", label, hex, ok ? "matched" : "REFUSED");
    return ok;
}

static bool BuildGuard_Check()
{
    return BuildGuard_ModuleMatches(g_exe, kSupportedExeSha256, "SOVIET64.exe") &&
           BuildGuard_ModuleMatches(g_engine, kSupportedEngineSha256, "C3DDLL64.dll");
}
