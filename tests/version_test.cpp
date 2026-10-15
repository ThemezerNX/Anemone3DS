// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/version.hpp"
#include "test.hpp"

#include <iterator>

using namespace tz;

namespace
{

// The parsed version, or 0.0.0-invalid, which makes a failed parse show up in the check that uses it.
Version parsed(std::string_view text)
{
    return Version::parse(text).value_or(Version{0, 0, 0, "invalid"});
}

} // namespace

TEST(version_parse)
{
    const auto version = Version::parse("4.1.2");
    REQUIRE(version.has_value());
    CHECK_EQ(version->major, 4);
    CHECK_EQ(version->minor, 1);
    CHECK_EQ(version->patch, 2);
    CHECK(version->prerelease.empty());

    // a leading 'v', as in git tags
    CHECK(parsed("v4.1.2") == (Version{4, 1, 2, ""}));
    CHECK(parsed("V4.1.2") == (Version{4, 1, 2, ""}));
    // missing components are 0
    CHECK(parsed("4") == (Version{4, 0, 0, ""}));
    CHECK(parsed("v4.1") == (Version{4, 1, 0, ""}));
    CHECK(parsed("01.002.0003") == (Version{1, 2, 3, ""}));
}

TEST(version_parse_prerelease_and_build)
{
    // what the Makefile makes of an untagged commit
    const auto dev = Version::parse("4.0.0-dev+abc1234");
    REQUIRE(dev.has_value());
    CHECK(*dev == (Version{4, 0, 0, "dev"}));

    CHECK_EQ(parsed("1.0.0-beta.2").prerelease, std::string("beta.2"));
    CHECK_EQ(parsed("1.0.0-x-y.7z.--").prerelease, std::string("x-y.7z.--"));
    CHECK_EQ(parsed("4.1-rc.1").prerelease, std::string("rc.1"));
    // leading zeros in numeric identifiers go, so that equal versions are equal values
    CHECK_EQ(parsed("1.0.0-rc.01").prerelease, std::string("rc.1"));
    CHECK_EQ(parsed("1.0.0-rc.00").prerelease, std::string("rc.0"));
    // build metadata is ignored, even with a '-' in it
    CHECK(parsed("4.0.0+abc1234") == (Version{4, 0, 0, ""}));
    CHECK(parsed("4.0.0+build-5") == (Version{4, 0, 0, ""}));
    CHECK(parsed("4.0.0-rc.1+build.7") == (Version{4, 0, 0, "rc.1"}));
}

TEST(version_parse_invalid)
{
    const char* invalid[] = {
        "",       "v",          "vv1.0.0",       "x1.0.0", "1.0.0.0", "1..0",    "1.",          ".1",       "1.0.0-",
        "-1.0.0", "1.0.0-a..b", "1.0.0-beta!",   "1.x.0",  "1.0.0 ",  " 1.0.0",  "1.0.0-be ta", "1.-1.0",   "+1.0.0",
        "1.0.+1", "1.0.0-.a",   "99999999999.0", "1e3",    "0x10",    "1.0.0-é", "1,0,0",       "1.0.0-a.",
    };
    for (const char* text : invalid)
    {
        if (Version::parse(text).has_value())
            test::fail(__FILE__, __LINE__, std::string("parsed \"") + text + "\"");
    }
}

TEST(version_order)
{
    // SemVer's own example of precedence, then releases
    const char* ascending[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1",  "1.0.0",         "1.0.1",            "1.1.0",      "1.10.0",       "2.0.0",
        "10.0.0",
    };
    for (size_t i = 0; i + 1 < std::size(ascending); ++i)
    {
        const Version lower = parsed(ascending[i]);
        const Version higher = parsed(ascending[i + 1]);
        const bool ordered = lower < higher && higher > lower && lower != higher &&
                             (lower <=> higher) == std::strong_ordering::less;
        if (!ordered)
            test::fail(__FILE__, __LINE__, std::string(ascending[i]) + " < " + ascending[i + 1]);
    }

    // a pre-release comes before its release, but after older releases
    CHECK(parsed("4.0.0-dev") < parsed("4.0.0"));
    CHECK(parsed("4.0.0-dev+abc1234") > parsed("3.9.9"));
    CHECK(parsed("v4.1.0") > parsed("4.0.9"));
    // numeric identifiers sort before words, and by value however long they are
    CHECK(parsed("1.0.0-9") < parsed("1.0.0-a"));
    CHECK(parsed("1.0.0-10") > parsed("1.0.0-9"));
    CHECK(parsed("1.0.0-123456789012345678901") > parsed("1.0.0-99999999999999999999"));
    CHECK(parsed("1.0.0-alpha") < parsed("1.0.0-alpha.0"));
    // words in ASCII order
    CHECK(parsed("1.0.0-Beta") < parsed("1.0.0-beta"));
    CHECK(parsed("1.0.0-beta-2") > parsed("1.0.0-beta"));
}

TEST(version_equality)
{
    // build metadata and a 'v' make no difference
    CHECK((parsed("4.0.0+a") <=> parsed("v4.0.0+b")) == std::strong_ordering::equal);
    CHECK(parsed("4.0.0+a") == parsed("v4.0.0+b"));
    CHECK(parsed("1.0.0-rc.01") == parsed("1.0.0-rc.1"));
    CHECK((parsed("1.0.0-rc.01") <=> parsed("1.0.0-rc.1")) == std::strong_ordering::equal);
    CHECK(parsed("4.0") == parsed("4.0.0"));
    CHECK(parsed("4.0.0-dev") != parsed("4.0.0"));
    CHECK(parsed("4.0.0-alpha") != parsed("4.0.0-beta"));
    // a version made by hand with leading zeros still compares by value
    CHECK((Version{1, 0, 0, "01"} <=> Version{1, 0, 0, "2"}) == std::strong_ordering::less);
}

TEST(version_to_string)
{
    CHECK_EQ(parsed("4.1.2").toString(), std::string("4.1.2"));
    CHECK_EQ(parsed("v4.1").toString(), std::string("4.1.0"));
    CHECK_EQ(parsed("4.0.0-dev+abc1234").toString(), std::string("4.0.0-dev"));
    CHECK_EQ(parsed("1.2.3-beta.02").toString(), std::string("1.2.3-beta.2"));

    // what toString() gives reads back as the same version
    for (const char* text : {"0.0.1", "4.0.0-dev", "v10.20.30-rc.1.x-y", "1.0.0-alpha.beta+exp.sha.5114f85"})
    {
        const Version version = parsed(text);
        if (!(parsed(version.toString()) == version))
            test::fail(__FILE__, __LINE__, std::string("round trip of ") + text);
    }
}
