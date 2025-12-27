#pragma once
#include <string>
#include <cstdint>

// FNV-1a constants for 64-bit
const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325;
const uint64_t FNV_PRIME = 0x100000001b3;

inline uint64_t fnv1a_hash(const std::string& str) {
    uint64_t hash = FNV_OFFSET_BASIS;

    for (unsigned char c : str) {
        hash ^= c;
        hash *= FNV_PRIME;
    }

    return hash;
}
