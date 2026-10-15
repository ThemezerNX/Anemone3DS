// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// A small unit test runner for the parts of the app that don't need a 3DS. `make test` builds every .cpp in tests/
// with the portable sources and runs them all.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace test
{

struct Case
{
    const char* name;
    void (*run)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);

struct Registrar
{
    Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

template <class T>
std::string show(const T& value)
{
    if constexpr (requires { std::to_string(value); })
        return std::to_string(value);
    else if constexpr (requires { std::string(value); })
        return "\"" + std::string(value) + "\"";
    else
        return "?";
}

} // namespace test

#define TEST(name)                                                                                                     \
    static void test_##name();                                                                                         \
    static ::test::Registrar test_registrar_##name(#name, test_##name);                                                \
    static void test_##name()

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            ::test::fail(__FILE__, __LINE__, #expr);                                                                   \
    } while (0)

// Stops the test when it fails, for checks that later ones depend on.
#define REQUIRE(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            ::test::fail(__FILE__, __LINE__, #expr);                                                                   \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto& test_actual_ = (actual);                                                                           \
        const auto& test_expected_ = (expected);                                                                       \
        if (!(test_actual_ == test_expected_))                                                                         \
            ::test::fail(__FILE__, __LINE__,                                                                           \
                         std::string(#actual " == " #expected ", got ") + ::test::show(test_actual_) +                 \
                             " instead of " + ::test::show(test_expected_));                                           \
    } while (0)
