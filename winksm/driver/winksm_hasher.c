#include "winksm.h"

/*
 * xxhash32 - fast non-cryptographic hash
 * Ported from Linux KSM's use of xxhash for page content hashing.
 * Based on Yann Collet's xxHash specification.
 */

#define XXH_PRIME1 0x9E3779B1U
#define XXH_PRIME2 0x85EBCA77U
#define XXH_PRIME3 0xC2B2AE3DU
#define XXH_PRIME4 0x27D4EB2FU
#define XXH_PRIME5 0x165667B1U

static uint32_t XXH32_round(uint32_t seed, uint32_t input) {
    seed += input * XXH_PRIME2;
    seed  = (seed << 13) | (seed >> 19);
    seed *= XXH_PRIME1;
    return seed;
}

static uint32_t XXH32_mergeRound(uint32_t acc, uint32_t val) {
    val  = XXH32_round(0, val);
    acc ^= val;
    acc  = acc * XXH_PRIME1 + XXH_PRIME4;
    return acc;
}

static uint32_t XXH32_avalanche(uint32_t hash) {
    hash ^= hash >> 15;
    hash *= XXH_PRIME2;
    hash ^= hash >> 13;
    hash *= XXH_PRIME3;
    hash ^= hash >> 16;
    return hash;
}

uint32_t WinKSM_Hash_xxhash32(const void *Data, size_t Length) {
    const uint8_t *p = (const uint8_t *)Data;
    const uint8_t *end = p + Length;
    uint32_t hash;

    if (Length >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = XXH_PRIME1 + XXH_PRIME2;
        uint32_t v2 = XXH_PRIME2;
        uint32_t v3 = 0;
        uint32_t v4 = (uint32_t)(-(int32_t)XXH_PRIME1);

        do {
            v1 = XXH32_round(v1, *(const uint32_t *)(p + 0));
            v2 = XXH32_round(v2, *(const uint32_t *)(p + 4));
            v3 = XXH32_round(v3, *(const uint32_t *)(p + 8));
            v4 = XXH32_round(v4, *(const uint32_t *)(p + 12));
            p += 16;
        } while (p <= limit);

        hash = (v1 << 1) | (v1 >> 31);
        hash += XXH32_mergeRound(hash, v2);
        hash += XXH32_mergeRound(hash, v3);
        hash += XXH32_mergeRound(hash, v4);
    } else {
        hash = XXH_PRIME5;
    }

    hash += (uint32_t)Length;

    while (p + 4 <= end) {
        hash += *(const uint32_t *)p * XXH_PRIME3;
        hash  = (hash << 17) | (hash >> 15);
        hash *= XXH_PRIME4;
        p += 4;
    }

    while (p < end) {
        hash += (*p) * XXH_PRIME5;
        hash  = (hash << 11) | (hash >> 21);
        hash *= XXH_PRIME1;
        p++;
    }

    return XXH32_avalanche(hash);
}

int WinKSM_ComparePages(const void *Page1, const void *Page2) {
    return RtlCompareMemory(Page1, Page2, WINKSM_PAGE_SIZE) == WINKSM_PAGE_SIZE ? 0 : 1;
}
