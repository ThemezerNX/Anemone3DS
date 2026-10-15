// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/result.hpp"

#include <cstdint>

namespace tz::act
{

// The principal ID of the console's Nintendo Network ID, which the badge files store.
// Fails when the console has no NNID or the account service can't be used.
Expected<uint32_t> principalId();

} // namespace tz::act
