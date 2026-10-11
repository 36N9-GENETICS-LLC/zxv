/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* test_hwp.c — host tests for ZHP-1 hardware profiles (hwp.h P1-P3). */
#include <stdio.h>
#include <string.h>
#include "hwp.h"

static int fails, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

/* A commodity arm64 laptop, as the host probe would describe it. */
static void laptop(hwp_profile_t *p)
{
    uint16_t i;
    hwp_init(p);
    hwp_node_add(p, 1, HWP_ROOT, HWP_CLASS_MACHINE, &i);
    hwp_node_add(p, 2, 1, HWP_CLASS_CPU_CLUSTER, &i);
    hwp_cap_set(p, i, HWP_CAP_COUNT, 16);
    hwp_cap_set(p, i, HWP_CAP_WORD_BITS, 64);
    hwp_cap_set(p, i, HWP_CAP_ISA, HWP_ISA_ARM64);
    hwp_node_add(p, 3, 1, HWP_CLASS_MEMORY, &i);
    hwp_cap_set(p, i, HWP_CAP_BYTES, 128ull << 30);
    hwp_node_add(p, 4, 2, HWP_CLASS_VECTOR_UNIT, &i);
    hwp_cap_set(p, i, HWP_CAP_INT_WIDTHS, HWP_INT_8 | HWP_INT_16 | HWP_INT_32 | HWP_INT_64);
    hwp_cap_set(p, i, HWP_CAP_SIMD, HWP_SIMD_NEON | HWP_SIMD_DOTPROD);
    hwp_node_add(p, 5, 2, HWP_CLASS_CACHE, &i);
    hwp_cap_set(p, i, HWP_CAP_LINE_BYTES, 128);
}

int main(void)
{
    static hwp_profile_t a, b, c;
    static uint8_t buf[8192], buf2[8192];
    uint32_t n, n2;
    uint16_t idx;

    /* P1: order and parents are enforced as the profile is built. */
    hwp_init(&c);
    CHECK(hwp_node_add(&c, 5, HWP_ROOT, HWP_CLASS_MACHINE, &idx) == HWP_OK);
    CHECK(hwp_node_add(&c, 4, 5, HWP_CLASS_CPU_CORE, 0) == HWP_E_CANONICAL);
    CHECK(hwp_node_add(&c, 6, 9, HWP_CLASS_CPU_CORE, 0) == HWP_E_CANONICAL);
    CHECK(hwp_node_add(&c, 6, 5, 0, 0) == HWP_E_ARG);
    /* P2: caps land sorted whatever order they are set in. */
    CHECK(hwp_cap_set(&c, idx, HWP_CAP_BYTES, 1) == HWP_OK);
    CHECK(hwp_cap_set(&c, idx, HWP_CAP_WORD_BITS, 64) == HWP_OK);
    CHECK(c.node[0].cap[0].key == HWP_CAP_WORD_BITS);
    CHECK(hwp_cap_set(&c, idx, (uint16_t) (HWP_CMP_EXACT | 1u), 64) == HWP_E_DUPLICATE);

    /* Round trip, one encoding, one digest. */
    laptop(&a);
    CHECK(hwp_encode(&a, buf, sizeof buf, &n) == HWP_OK && n == hwp_encoded_len(&a));
    CHECK(hwp_decode(buf, n, &b) == HWP_OK);
    CHECK(hwp_encode(&b, buf2, sizeof buf2, &n2) == HWP_OK && n2 == n && !memcmp(buf, buf2, n));
    uint8_t d1[32], d2[32];
    hwp_digest(&a, d1);
    hwp_digest(&b, d2);
    CHECK(!memcmp(d1, d2, 32));
    CHECK(hwp_encode(&a, buf2, n - 1, &n2) == HWP_E_BUFFER && n2 == n);

    /* State blobs travel with the node and change the digest. */
    static const uint8_t st[] = {1, 2, 3};
    CHECK(hwp_state_set(&b, 1, st, 3) == HWP_OK);
    hwp_digest(&b, d2);
    CHECK(memcmp(d1, d2, 32) != 0);
    CHECK(hwp_encode(&b, buf2, sizeof buf2, &n2) == HWP_OK && hwp_decode(buf2, n2, &c) == HWP_OK);
    CHECK(c.node[1].state_len == 3 && c.node[1].state[2] == 3);

    /* P3 and format errors. */
    CHECK(hwp_decode(buf, n - 1, &c) == HWP_E_CANONICAL);
    memcpy(buf2, buf, n);
    buf2[3] = '2';
    CHECK(hwp_decode(buf2, n, &c) == HWP_E_FORMAT);
    memcpy(buf2, buf, n);
    buf2[6] = 1; /* flags must be zero */
    CHECK(hwp_decode(buf2, n, &c) == HWP_E_CANONICAL);
    /* Swap the first two cap keys of node 2 (P2). */
    memcpy(buf2, buf, n);
    {
        uint32_t o = HWP_HEADER + 12 + 0 /* node 1 has no caps */ + 12;
        uint8_t t[10];
        memcpy(t, buf2 + o, 10);
        memcpy(buf2 + o, buf2 + o + 10, 10);
        memcpy(buf2 + o + 10, t, 10);
        CHECK(hwp_decode(buf2, n, &c) == HWP_E_CANONICAL);
    }
    /* Truncated mid-node. */
    memcpy(buf2, buf, n);
    buf2[12] = (uint8_t) (HWP_HEADER + 5);
    buf2[13] = buf2[14] = buf2[15] = 0;
    CHECK(hwp_decode(buf2, HWP_HEADER + 5, &c) == HWP_E_FORMAT);

    /* Matching against R1: a laptop runs CPU, memory and vectors natively
     * and emulates the matrix, ternary, hash, crypto, fabric, clock,
     * secure element and wire units. */
    const hwp_profile_t *r1 = hwp_reference_r1();
    CHECK(r1->n == 12);
    hwp_match_t mt;
    CHECK(hwp_match(&a, r1, &mt) == HWP_OK);
    CHECK(mt.native == 3 && mt.emulated == 8);
    CHECK(mt.pair[0].kind == HWP_M_GROUP);
    CHECK(mt.pair[1].kind == HWP_M_NATIVE && mt.pair[1].have_id == 2);
    CHECK(mt.pair[3].kind == HWP_M_NATIVE && mt.pair[3].have_id == 4);
    CHECK(mt.pair[4].kind == HWP_M_EMULATE && mt.pair[4].unmet_key == 0);

    /* A machine built to R1 (here a profile copied from R1 with bigger
     * numbers) runs everything natively. A private class and a private,
     * non-critical cap ride along without being understood. */
    static hwp_profile_t chip;
    memcpy(&chip, r1, sizeof chip);
    hwp_cap_set(&chip, 4, HWP_CAP_MACS_PER_CYCLE, 4096);
    hwp_cap_set(&chip, 4, (uint16_t) (HWP_CMP_INFO | 0x1234u), 77);
    CHECK(hwp_node_add(&chip, 200, 1, HWP_CLASS_PRIVATE | 7u, &idx) == HWP_OK);
    CHECK(hwp_match(&chip, r1, &mt) == HWP_OK && mt.native == 11 && mt.emulated == 0);
    /* Too few MACs: emulated, and the report says which cap. */
    hwp_cap_set(&chip, 4, HWP_CAP_MACS_PER_CYCLE, 8);
    CHECK(hwp_match(&chip, r1, &mt) == HWP_OK && mt.pair[4].kind == HWP_M_EMULATE &&
          mt.pair[4].unmet_key == HWP_CAP_MACS_PER_CYCLE);
    hwp_cap_set(&chip, 4, HWP_CAP_MACS_PER_CYCLE, 4096);
    /* A critical cap this reader does not know: not used blind. */
    hwp_cap_set(&chip, 5, (uint16_t) (HWP_KEY_CRITICAL | HWP_CMP_INFO | 0x1001u), 1);
    CHECK(hwp_match(&chip, r1, &mt) == HWP_OK && mt.pair[5].kind == HWP_M_EMULATE);
    /* Binary hardware never passes for ternary. */
    hwp_profile_t *tw = &c;
    memcpy(tw, r1, sizeof *tw);
    hwp_cap_set(tw, 5, HWP_CAP_RADIX, 2);
    CHECK(hwp_match(tw, r1, &mt) == HWP_OK && mt.pair[5].kind == HWP_M_EMULATE);

    CHECK(!strcmp(hwp_class_name(HWP_CLASS_TERNARY_UNIT), "ternary-unit"));
    CHECK(!strcmp(hwp_class_name(HWP_CLASS_PRIVATE | 3u), "private"));

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
