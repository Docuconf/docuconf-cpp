#include <gtest/gtest.h>

#include "docuconf/duration.hpp"

using docuconf::Duration;
using docuconf::DurationEncoding;
using namespace std::chrono_literals;

namespace {

Duration ok(DurationEncoding e, const std::string& s) {
    auto d = docuconf::parse_duration(e, s);
    EXPECT_TRUE(d.has_value()) << s;
    return d.value_or(Duration(-1));
}

TEST(Duration, CanonicalGoForm) {
    EXPECT_EQ(docuconf::format_go_duration(90s), "1m30s");
    EXPECT_EQ(docuconf::format_go_duration(5400s), "1h30m");
    EXPECT_EQ(docuconf::format_go_duration(720h), "720h");
    EXPECT_EQ(docuconf::format_go_duration(1500ms), "1s500ms");
    EXPECT_EQ(docuconf::format_go_duration(90us), "90us");
    EXPECT_EQ(docuconf::format_go_duration(0s), "0s");
}

TEST(Duration, Go) {
    EXPECT_EQ(ok(DurationEncoding::Go, "1h2m3s4ms"), 3723004ms);
    EXPECT_EQ(ok(DurationEncoding::Go, "1.5h"), 5400s);
    EXPECT_EQ(ok(DurationEncoding::Go, "5ns"), Duration(5));
    EXPECT_EQ(ok(DurationEncoding::Go, "0"), Duration(0));
    for (const char* bad : {"", " 1s", "1s\n", "abc", "1", "-1s", "1d", "PT90S"})
        EXPECT_FALSE(docuconf::parse_duration(DurationEncoding::Go, bad)) << bad;
}

TEST(Duration, Iso8601) {
    EXPECT_EQ(ok(DurationEncoding::Iso8601, "PT90S"), 90s);
    EXPECT_EQ(ok(DurationEncoding::Iso8601, "PT1.5S"), 1500ms);
    EXPECT_EQ(ok(DurationEncoding::Iso8601, "PT0.001S"), 1ms);
    EXPECT_EQ(ok(DurationEncoding::Iso8601, "P1DT2H3M4S"), 93784s);
    EXPECT_EQ(ok(DurationEncoding::Iso8601, "PT0S"), 0s);
    for (const char* bad : {"P", "PT", "1m30s", "PT1H30", "P1Y", "PT-1S", " PT1S", "90", "PT1S2M"})
        EXPECT_FALSE(docuconf::parse_duration(DurationEncoding::Iso8601, bad)) << bad;
}

TEST(Duration, Seconds) {
    EXPECT_EQ(ok(DurationEncoding::Seconds, "90"), 90s);
    EXPECT_EQ(ok(DurationEncoding::Seconds, "0.25"), 250ms);
    for (const char* bad : {"90s", "", "-1", "1e3", ".5", "1.", " 1"})
        EXPECT_FALSE(docuconf::parse_duration(DurationEncoding::Seconds, bad)) << bad;
}

TEST(Duration, Timespan) {
    EXPECT_EQ(ok(DurationEncoding::Timespan, "00:01:30"), 90s);
    EXPECT_EQ(ok(DurationEncoding::Timespan, "1.02:03:04.5"), 93784500ms);
    EXPECT_EQ(ok(DurationEncoding::Timespan, "2.00:00:00"), 172800s);
    for (const char* bad : {"1m30s", "24:00:00", "00:60:00", "00:00:60", "1:30", "00:01:30.12345678"})
        EXPECT_FALSE(docuconf::parse_duration(DurationEncoding::Timespan, bad)) << bad;
}

}  // namespace
