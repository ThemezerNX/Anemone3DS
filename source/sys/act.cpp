// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "sys/act.hpp"

#include <3ds.h>

namespace tz::act
{

namespace
{

// The SDK version ACT is told this process was built with; the account service wants one it knows.
constexpr u32 kSdkVersion = 0xB0502C8;

} // namespace

Expected<uint32_t> principalId()
{
    // act:u, the user service, which homebrew can open; act:a is for system titles.
    Result result = actInit(true);
    Expected<uint32_t> id = fail(Errc::NotAllowed, "actInit", result);
    if (R_SUCCEEDED(result))
    {
        result = ACT_Initialize(kSdkVersion, 0, 0);
        if (R_FAILED(result))
        {
            id = fail(Errc::NotAllowed, "ACT_Initialize", result);
        }
        else
        {
            u32 value = 0;
            result = ACT_GetAccountInfo(&value, sizeof(value), ACT_DEFAULT_ACCOUNT, INFO_TYPE_PRINCIPAL_ID);
            if (R_FAILED(result))
                id = fail(Errc::NotFound, "ACT_GetAccountInfo", result);
            else if (value == 0)
                id = fail(Errc::NotFound, "no Nintendo Network ID");
            else
                id = value;
        }
    }
    // also after a failed actInit, which leaves the service's reference count raised and would make the next
    // actInit "succeed" without a session
    actExit();
    return id;
}

} // namespace tz::act
