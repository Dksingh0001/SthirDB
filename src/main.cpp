#include <cstdio>
#include <string>

#include "common/config.h"

static void PrintUsage() {
  std::printf(
      "SthirDB %s — disk-resident relational engine\n"
      "\n"
      "Usage:\n"
      "  sthirdb                     Print this help and exit (status 0).\n"
      "  sthirdb --version           Print version and exit.\n"
      "  sthirdb init_schema         Create ./data/retail and the Items table file.\n"
      "  sthirdb load_data --rows N  Bulk-insert N rows into Items.\n"
      "  sthirdb count               Count live rows in Items.\n",
      sthir::kVersion);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 0;
  }
  const std::string cmd = argv[1];
  if (cmd == "--version" || cmd == "-v") {
    std::printf("SthirDB %s\n", sthir::kVersion);
    return 0;
  }
  std::fprintf(stderr, "sthirdb: unknown command '%s'\n", cmd.c_str());
  PrintUsage();
  return 2;
}
