// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tesmio_plugin.h"

// Test-copy isolation only. Call after the supported-build guard, before the
// game's worker constructors (the suspended-process injection path does this).
// Requires TEST-COPY.txt beside EXE and RepublicMP.qa.enabled plus
// RepublicMP.qa.isolate-events.enabled beside this plugin before Bind.
void QaInstanceEvents_Bind(const TsmHost* host);
