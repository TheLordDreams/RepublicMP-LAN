// SPDX-License-Identifier: GPL-3.0-only
#pragma once

enum NetworkSetting {
    NET_CONNECT_SECONDS, NET_HELLO_SECONDS, NET_TRANSFER_SECONDS,
    NET_WORLD_SECONDS, NET_REJOIN_SECONDS, NET_SILENCE_SECONDS,
    NET_SEND_SECONDS, NET_SETTING_COUNT
};
struct NetworkSettingSpec { const char* key; const char* label; int initial, low, high; };
inline constexpr NetworkSettingSpec kNetworkSettings[] = {
    {"connect_seconds", "Connection timeout (s)", 15, 5, 120},
    {"hello_seconds", "Handshake message timeout (s)", 15, 5, 120},
    {"transfer_seconds", "Transfer block timeout (s)", 120, 15, 600},
    {"world_seconds", "World preparation wait (s)", 600, 60, 1800},
    {"rejoin_seconds", "Re-sync reconnect window (s)", 30, 15, 180},
    {"silence_seconds", "Silent player timeout (s)", 20, 10, 120},
    {"send_seconds", "Blocked game send timeout (s)", 2, 1, 5}
};
inline int NetworkSettings_Clamp(int setting, int value) {
    const auto& spec = kNetworkSettings[setting];
    return value < spec.low ? spec.low : value > spec.high ? spec.high : value;
}
// Loaded with the profile; UI changes are allowed only between sessions.
int NetworkSettings_Get(int setting);
void NetworkSettings_Set(int setting, int seconds);
inline unsigned long NetworkSettings_Ms(int setting) { return (unsigned long)NetworkSettings_Get(setting) * 1000; }
