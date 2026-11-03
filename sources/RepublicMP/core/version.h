// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP version and wire-protocol number.
//
// REPUBLICMP_VERSION: MAJOR.FEATURE.PATCH (see README.md's Versioning section).
// REPUBLICMP_PROTOCOL: bump whenever any network message layout changes.
//
// When a player joins, both sides exchange the version, the protocol and a
// CRC32 of their own RepublicMP.dll file (session/save_sync); any difference
// refuses the join, so two different builds never exchange messages.
#pragma once
#include "feature_config.h"

#if REPUBLICMP_TCP_ONLY
#define REPUBLICMP_VERSION "0.7.2-lan2-rc1"
#else
#define REPUBLICMP_VERSION "0.7.2-loc1"
#endif
#define REPUBLICMP_PROTOCOL 51
