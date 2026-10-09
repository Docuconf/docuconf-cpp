// Durations in the wire encodings of SPEC §5, and the canonical Go form.
#pragma once

#include <optional>
#include <string>

#include "spec.hpp"

namespace docuconf {

/// Parses a Go duration such as `1m30s`, `1h2m3s4ms`, `250ms`, `1.5h` or
/// `-5s`, exactly as Go's time.ParseDuration does (SPEC §5).
std::optional<Duration> parse_go_duration(const std::string& s);

/// Parses a duration in one of the wire encodings: `go` (`1m30s`),
/// `iso8601` (`PT90S`, `PT1.5S`, `P1DT2H`), `seconds` (`90`, `0.25`) or
/// `timespan` (`00:01:30`, `1.02:03:04.5`).
std::optional<Duration> parse_duration(DurationEncoding encoding, const std::string& s);

/// Formats a duration in canonical Go form: `1h30m`, `1m30s`, `1s500ms`,
/// `0s`. Each unit at most once, largest first, zero units left out.
std::string format_go_duration(Duration d);

}  // namespace docuconf
