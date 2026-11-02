// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tesmio_plugin.h"

struct IDXGISwapChain;

// Private QA only: requires TEST-COPY.txt beside the executable and
// RepublicMP.qa.enabled beside this plugin. A one-shot request beside the
// plugin produces RepublicMP.qa.frame.bmp, never a desktop screenshot.
void QaFrameCapture_Bind(const TsmHost* host);
// Call on the game's render thread, after drawing overlays, before Present.
void QaFrameCapture_Tick(IDXGISwapChain* swapChain);
