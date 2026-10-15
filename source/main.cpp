// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "app/app.hpp"

int main(int argc, char** argv)
{
    return tz::App::get().run(argc, argv);
}
