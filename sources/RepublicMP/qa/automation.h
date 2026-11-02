// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tesmio_plugin.h"

// Inert unless BOTH explicit QA marker files exist. Bind after Determinism_Bind;
// Tick from the main game's Present frame, once per frame.
void QaAutomation_Bind(const TsmHost* host);
void QaAutomation_Tick();
// Test-only loopback lockstep proxy, inert without explicit copy/QA/proxy markers.
unsigned short QaAutomation_ConnectPort(const char* hostIp, unsigned short port);
