// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tesmio_plugin.h"

// Diagnostic only. Bind after the executable fingerprint guard and subsystem
// binds. A third opt-in marker is required before this hook is installed.
void QaVehicleTrace_Bind(const TsmHost* host);
// Call from Present. All allocation, formatting and file I/O happens here.
void QaVehicleTrace_Flush();
