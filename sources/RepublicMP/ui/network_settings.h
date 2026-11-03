// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "imgui.h"
#include "ui_text.h"
#include "../core/network_settings.h"
#include "../session/save_sync.h"

inline void NetworkSettings_DrawControls()
{
    ImGui::TextWrapped("%s", T("Change network settings before hosting or joining."));
    ImGui::BeginDisabled(SaveSync_IsBusy());
    for (int i = 0; i < NET_SETTING_COUNT; ++i) {
        ImGui::PushID(i);
        int value = NetworkSettings_Get(i);
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderInt(T(kNetworkSettings[i].label), &value, kNetworkSettings[i].low, kNetworkSettings[i].high, "%d", ImGuiSliderFlags_AlwaysClamp)) NetworkSettings_Set(i, value);
        ImGui::PopID();
    }
    if (ImGui::Button(T("Restore network defaults")))
        for (int i = 0; i < NET_SETTING_COUNT; ++i) NetworkSettings_Set(i, kNetworkSettings[i].initial);
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", T("Longer timeouts help slow connections; they do not fix simulation desyncs."));
    ImGui::Separator();
    static const int delays[] = {0, 60, 120, 300, -1};
    const char* labels[] = {T("Immediately"), T("After 1 minute"), T("After 2 minutes"), T("After 5 minutes"), T("Manual only")};
    int selected = 1;
    for (int i = 0; i < 5; ++i) if (delays[i] == SaveSync_GetDesyncResyncSeconds()) selected = i;
    ImGui::BeginDisabled(SaveSync_IsBusy() && !SaveSync_IsHostRole());
    if (ImGui::Combo(T("After simulation desync"), &selected, labels, 5)) SaveSync_SetDesyncResyncSeconds(delays[selected]);
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", T("The host re-sends the city after a mismatch. Manual mode leaves the worlds different until Re-sync now."));
}
