#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

// Minimal assert-style test support: CHECK aborts the test binary with a
// message and exit code 1 on failure; exit 0 means every CHECK passed.
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    const auto va = (a);                                                         \
    const auto vb = (b);                                                         \
    if (!(va == vb)) {                                                           \
      std::fprintf(stderr, "CHECK_EQ failed: %s == %s (%s:%d)\n", #a, #b, __FILE__, \
                   __LINE__);                                                    \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

inline void test_passed(const char* name) { std::printf("PASS %s\n", name); }
