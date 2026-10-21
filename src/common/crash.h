#pragma once

namespace sthir {

// Crash-injection hook for Novelty 2 (Phase 5B).
//
// No-op in normal runs. When the environment variable STHIRDB_CRASH_POINT is
// set to `name`, the process hard-exits (_exit, no flush, no destructors) so
// the on-disk state is left exactly as a real crash would leave it. Every page
// write and (from Phase 5) every log append routes through this hook via a
// single low-level function, so no call sites change when tests inject crashes.
void crash_point(const char* name);

}  // namespace sthir
