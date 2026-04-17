#ifndef AFIS_RENAMER_H
#define AFIS_RENAMER_H

#include <cstdint>

#include "ir.h"

namespace afis {

struct RenameResult {
    Program program;
    RenameMap renameMap;
};

RenameResult RenameRegisters(const Program& program, std::uint64_t seed);

}  // namespace afis

#endif