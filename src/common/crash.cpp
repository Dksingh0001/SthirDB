#include "common/crash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace sthir {

void crash_point(const char* name) {
  const char* target = std::getenv("STHIRDB_CRASH_POINT");
  if (target == nullptr || target[0] == '\0') return;
  if (std::strcmp(target, name) != 0) return;
  std::fprintf(stderr, "[crash_point] simulated crash at '%s'\n", name);
  ::_exit(99);  // no stdio flush, no stack unwinding — mirrors power loss
}

}  // namespace sthir
