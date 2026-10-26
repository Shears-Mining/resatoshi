// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit-license.php.

#ifndef BITCOIN_COMPAT_FORTIFY_H
#define BITCOIN_COMPAT_FORTIFY_H

#if defined(__linux__)
#include <features.h>

#if defined(__GLIBC__) && defined(__ELF__)
#include <cstdint>

// Optimisation can eliminate every fortified call, so imported *_chk symbols
// alone cannot establish whether fortification was enabled. Record the level
// selected by the libc headers in this translation unit, including level zero.
// The retained ELF note survives both section garbage collection and stripping.
[[gnu::used, gnu::retain, gnu::section(".note.rsat.fortify"), gnu::aligned(4)]]
static const struct {
    std::uint32_t name_size;
    std::uint32_t descriptor_size;
    std::uint32_t type;
    char name[8];
    std::uint32_t version;
    std::uint32_t level;
} fortify_note{5, 8, 0x52534154, "RSAT", 1, __USE_FORTIFY_LEVEL};

#endif // defined(__GLIBC__) && defined(__ELF__)
#endif // defined(__linux__)

#endif // BITCOIN_COMPAT_FORTIFY_H
