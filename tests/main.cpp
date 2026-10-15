// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "test.hpp"

#include <cstring>

namespace test
{

std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

static int failures = 0;
static bool currentFailed = false;

void fail(const char* file, int line, const std::string& message)
{
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
    ++failures;
    currentFailed = true;
}

} // namespace test

int main(int argc, char** argv)
{
    // an argument runs only the tests whose name contains it
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;
    for (const test::Case& c : test::registry())
    {
        if (filter && !std::strstr(c.name, filter))
            continue;
        test::currentFailed = false;
        c.run();
        ++run;
        if (test::currentFailed)
        {
            ++failed;
            std::fprintf(stderr, "FAILED %s\n", c.name);
        }
    }
    std::printf("%d tests, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
