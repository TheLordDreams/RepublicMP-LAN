// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tesmio_plugin.h"

// POD for the QA status JSON serializer. Values are observed game state;
// queued flags describe this process's submitted actions, never success.
struct QaProbeBuildingStatus {
    bool optedIn, worldReady, slotChosen, buildQueued, demolishQueued;
    bool exists, demolished;
    unsigned worldLoads;
    int slot, totalBuildings, nearbyBuildings, index, category;
    float x, y, z, progress;
    int payRub, payUsd, stopped, demolishing, localRub, localUsd;
    double rubles, dollars;
    unsigned long long hashBuildings;
};

void QaConstructionProbe_Bind(const TsmHost* host);
// Main/game thread only, paused connected lockstep. Before any placement,
// call GetProbeBuildingStatus for both slots on BOTH peers to prime equal
// target choices. Targets reset on world load; repeat priming after a load.
bool GetProbeBuildingStatus(int slot, QaProbeBuildingStatus* out);
bool QueueTestBuilding(int slot);
bool QueueTestDemolish(int slot);
const char* QaConstructionProbe_LastError();
