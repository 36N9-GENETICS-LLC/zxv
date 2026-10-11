/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* test_zxv_abi14.c — conformance test of ABI 1.4 (hardware emulation and
 * hardware updates), linked against the shared library only:
 *   - ZHP-1 profiles: host probe, reference R1, canonical round trip,
 *     digest, matching;
 *   - the emulation matrix: proof before service, failover, dependency
 *     calls back into the library, shadow disputes, the memo;
 *   - reconfigurable regions: signed images built and loaded through a
 *     simulated two-slot device, conformance failure and S0 quarantine,
 *     replay refusal, the quarantine log, fail-closed when it fills;
 *   - capability gating and the new status codes. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zxv_api.h"

static int checks, failures;
#define CHECK(c, ...)                                     \
    do {                                                  \
        checks++;                                         \
        if (!(c)) {                                       \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)
#define OK(call)                                                                   \
    do {                                                                           \
        int32_t rc_ = (call);                                                      \
        CHECK(rc_ == ZXV_OK, "%s -> %s", #call, zxv_status_name(rc_));             \
    } while (0)
#define IS(call, want)                                                                         \
    do {                                                                                       \
        int32_t rc_ = (call);                                                                  \
        CHECK(rc_ == (want), "%s -> %s, want %s", #call, zxv_status_name(rc_), #want);         \
    } while (0)

/* ===================================================================== */
/* 1. Profiles                                                           */
/* ===================================================================== */

static void test_profiles(void)
{
    zxv_hw_profile *host, *r1, *copy, *mine;
    OK(zxv_hw_profile_probe_host(&host));
    OK(zxv_hw_profile_reference(&r1));
    uint32_t n;
    OK(zxv_hw_profile_count(r1, &n));
    CHECK(n == 12, "R1 has %u nodes", n);
    OK(zxv_hw_profile_count(host, &n));
    CHECK(n >= 2, "host profile has %u nodes", n);
    uint64_t v = 0;
    OK(zxv_hw_profile_cap_get(host, 2, ZXV_HW_CAP_WORD_BITS, &v));
    CHECK(v == sizeof(void *) * 8, "word bits %llu", (unsigned long long) v);
    OK(zxv_hw_profile_cap_get(host, 2, ZXV_HW_CAP_COUNT, &v));
    CHECK(v >= 1, "cores %llu", (unsigned long long) v);

    /* The host runs the CPU natively and emulates what it lacks. */
    uint32_t nat = 0, emu = 0;
    OK(zxv_hw_profile_match(host, r1, &nat, &emu));
    CHECK(nat >= 1 && nat + emu == 11, "native %u emulated %u", nat, emu);
    uint32_t need, have, unmet;
    int32_t kind;
    OK(zxv_hw_profile_pair(host, r1, 1, &need, &kind, &have, &unmet));
    CHECK(need == 2 && kind == ZXV_HW_NATIVE && have == 2, "cpu pair %u %d %u", need, kind, have);
    OK(zxv_hw_profile_pair(host, r1, 5, &need, &kind, &have, &unmet));
    CHECK(kind == ZXV_HW_EMULATE, "a laptop has no ternary unit");
    IS(zxv_hw_profile_pair(host, r1, 99, &need, &kind, &have, &unmet), ZXV_E_NOT_FOUND);

    /* Canonical round trip and digest. */
    size_t len = 0;
    IS(zxv_hw_profile_encode(r1, NULL, 0, &len), ZXV_E_BUFFER);
    uint8_t *buf = malloc(len);
    OK(zxv_hw_profile_encode(r1, buf, len, &len));
    OK(zxv_hw_profile_decode(buf, len, &copy));
    uint8_t d1[32], d2[32];
    OK(zxv_hw_profile_digest(r1, d1));
    OK(zxv_hw_profile_digest(copy, d2));
    CHECK(!memcmp(d1, d2, 32), "digest differs after round trip");
    buf[len - 1] ^= 0; /* untouched */
    IS(zxv_hw_profile_decode(buf, len - 1, &mine), ZXV_E_FORMAT);
    free(buf);

    /* Build one: a machine built to R1 runs everything natively. */
    OK(zxv_hw_profile_create(&mine));
    OK(zxv_hw_profile_node_add(mine, 1, ZXV_HW_ROOT, ZXV_HW_CLASS_MACHINE));
    OK(zxv_hw_profile_node_add(mine, 2, 1, ZXV_HW_CLASS_TERNARY_UNIT));
    OK(zxv_hw_profile_cap_set(mine, 2, ZXV_HW_CAP_RADIX, 3));
    IS(zxv_hw_profile_node_add(mine, 2, 1, ZXV_HW_CLASS_CACHE), ZXV_E_ARG);
    IS(zxv_hw_profile_cap_set(mine, 9, ZXV_HW_CAP_RADIX, 3), ZXV_E_NOT_FOUND);
    OK(zxv_hw_profile_node_add(mine, 3, 1, ZXV_HW_CLASS_PRIVATE | 1u));
    OK(zxv_hw_profile_pair(mine, r1, 5, &need, &kind, &have, &unmet));
    CHECK(kind == ZXV_HW_NATIVE && have == 2, "ternary unit pairs natively");

    zxv_hw_profile_destroy(host);
    zxv_hw_profile_destroy(r1);
    zxv_hw_profile_destroy(copy);
    zxv_hw_profile_destroy(mine);
}

/* ===================================================================== */
/* 2. The matrix                                                         */
/* ===================================================================== */

typedef struct {
    uint8_t k;
    int bad;
} addk_t;

static int32_t addk(void *user, uint32_t op, const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                    size_t *out_len)
{
    (void) op;
    addk_t *a = user;
    if (a->bad == 2) return ZXV_E_IO;
    if (n > cap) return ZXV_E_BUFFER;
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t) (in[i] + a->k + (a->bad && in[i] == 9));
    *out_len = n;
    return ZXV_OK;
}

/* "add16" emulated through two calls to "add8" via the library itself. */
typedef struct {
    const zxv_cap *cap;
    zxv_hw_matrix *mx;
} chain_t;

static int32_t add_twice(void *user, uint32_t op, const uint8_t *in, size_t n, uint8_t *out,
                         size_t cap, size_t *out_len)
{
    chain_t *c = user;
    uint8_t tmp[64];
    size_t l = 0;
    int32_t rc = zxv_hw_call(c->cap, c->mx, 0x100, op, in, n, tmp, sizeof tmp, &l);
    if (rc) return rc;
    return zxv_hw_call(c->cap, c->mx, 0x100, op, tmp, l, out, cap, out_len);
}

static void test_matrix(zxv_cap *cap, zxv_cap *nohw)
{
    zxv_hw_matrix *mx;
    IS(zxv_hw_matrix_create_cap(nohw, &mx), ZXV_E_CAP);
    OK(zxv_hw_matrix_create_cap(cap, &mx));
    OK(zxv_hw_function_add(cap, mx, 0x100, "add8"));
    OK(zxv_hw_function_add(cap, mx, 0x101, "add16"));
    IS(zxv_hw_function_add(cap, mx, 0x100, "again"), ZXV_E_DUPLICATE);
    OK(zxv_hw_resource_add(cap, mx, 1)); /* CPU */
    OK(zxv_hw_resource_add(cap, mx, 2)); /* accelerator */
    const uint8_t in[] = {0, 1, 9, 200}, ex8[] = {8, 9, 17, 208}, ex16[] = {16, 17, 25, 216};
    OK(zxv_hw_vector_add(cap, mx, 0x100, 0, in, 4, ex8, 4));
    OK(zxv_hw_vector_add(cap, mx, 0x101, 0, in, 4, ex16, 4));

    addk_t fx = {8, 0}, sw = {8, 0};
    chain_t ch = {cap, mx};
    uint32_t pfx, psw, p16, pv;
    OK(zxv_hw_provider_add(cap, mx, 0x100, 2, ZXV_HW_KIND_FIXED, 1, addk, &fx, &pfx));
    OK(zxv_hw_provider_add(cap, mx, 0x100, 1, ZXV_HW_KIND_SOFT, 10, addk, &sw, &psw));
    OK(zxv_hw_provider_add(cap, mx, 0x101, 1, ZXV_HW_KIND_SOFT, 20, add_twice, &ch, &p16));
    IS(zxv_hw_provider_add(cap, mx, 0x100, 1, 9, 1, addk, &sw, &pv), ZXV_E_ARG);

    uint8_t out[64];
    size_t n = 0;
    IS(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n), ZXV_E_NOPROVIDER);
    int32_t lp;
    OK(zxv_hw_attest(cap, mx, pfx, &lp));
    CHECK(lp == ZXV_LP_TRUE, "fixed attest %d", lp);
    OK(zxv_hw_attest(cap, mx, psw, &lp));
    CHECK(lp == ZXV_LP_TRUE, "soft attest %d", lp);
    /* The chained provider calls add8 without declaring it: refused, so
     * its proof fails. Declared, it passes. */
    OK(zxv_hw_attest(cap, mx, p16, &lp));
    CHECK(lp == ZXV_LP_FALSE, "undeclared dependency must not prove (%d)", lp);
    OK(zxv_hw_provider_dep(cap, mx, p16, 0x100));
    OK(zxv_hw_attest(cap, mx, p16, &lp));
    CHECK(lp == ZXV_LP_TRUE, "chained attest %d", lp);
    OK(zxv_hw_call(cap, mx, 0x101, 0, in, 4, out, sizeof out, &n));
    CHECK(n == 4 && !memcmp(out, ex16, 4), "add16 output");

    OK(zxv_hw_bound(cap, mx, 0x100, &pv));
    CHECK(pv == pfx, "fixed unit serves (%u)", pv);
    OK(zxv_hw_resource_set(cap, mx, 2, ZXV_HW_RES_FAULTED));
    OK(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n));
    OK(zxv_hw_bound(cap, mx, 0x100, &pv));
    CHECK(pv == psw && !memcmp(out, ex8, 4), "failover to the emulator");
    OK(zxv_hw_resource_set(cap, mx, 2, ZXV_HW_RES_OK));

    /* A provider error comes back as its own status. */
    fx.bad = 2;
    IS(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n), ZXV_E_IO);
    fx.bad = 0;

    /* Shadow: agreement, then a silent fault -> dispute, nobody serves. */
    int32_t checked = 0;
    OK(zxv_hw_shadow(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n, &checked));
    CHECK(checked == 1, "shadow ran two providers");
    sw.bad = 1;
    IS(zxv_hw_shadow(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n, &checked), ZXV_E_DISPUTE);
    IS(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n), ZXV_E_NOPROVIDER);
    OK(zxv_hw_attest(cap, mx, pfx, &lp));
    CHECK(lp == ZXV_LP_TRUE, "fixed re-proves");
    OK(zxv_hw_attest(cap, mx, psw, &lp));
    CHECK(lp == ZXV_LP_FALSE, "faulty emulator fails its proof");

    /* Memo: a repeat skips the provider. */
    uint64_t hits, misses, calls0, calls1;
    IS(zxv_hw_memo(cap, mx, 0x100, 6, &hits, &misses), ZXV_E_ARG);
    OK(zxv_hw_memo(cap, mx, 0x100, 16, &hits, &misses));
    OK(zxv_hw_provider_info(cap, mx, pfx, NULL, NULL, NULL, &calls0, NULL));
    OK(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n));
    OK(zxv_hw_call(cap, mx, 0x100, 0, in, 4, out, sizeof out, &n));
    OK(zxv_hw_provider_info(cap, mx, pfx, NULL, NULL, NULL, &calls1, NULL));
    OK(zxv_hw_memo(cap, mx, 0x100, 16, &hits, &misses));
    CHECK(calls1 == calls0 + 1 && hits == 1 && misses == 1, "memo: calls %llu->%llu hits %llu",
          (unsigned long long) calls0, (unsigned long long) calls1, (unsigned long long) hits);
    CHECK(!memcmp(out, ex8, 4), "memo output");

    zxv_hw_matrix_destroy(mx);
}

/* ===================================================================== */
/* 3. Regions                                                            */
/* ===================================================================== */

typedef struct {
    uint8_t slot[2][256];
    size_t len[2];
    int active;
    int refuse;
} sim_t;

static int32_t sim_load(void *u, uint32_t s, const uint8_t *p, size_t n)
{
    sim_t *x = u;
    if (s > 1 || n > 256) return ZXV_E_RANGE;
    memcpy(x->slot[s], p, n);
    x->len[s] = n;
    return ZXV_OK;
}
static int32_t sim_activate(void *u, uint32_t s)
{
    sim_t *x = u;
    if (x->refuse) return ZXV_E_IO;
    x->active = (int) s;
    return ZXV_OK;
}
static int32_t sim_call(void *u, uint32_t op, const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                        size_t *out_len)
{
    (void) op;
    sim_t *x = u;
    if (x->active < 0 || n > cap) return ZXV_E_STATE;
    for (size_t i = 0; i < n; i++) out[i] = x->slot[x->active][in[i]];
    *out_len = n;
    return ZXV_OK;
}

/* The device must report the SHA-256 of what a slot holds. The public ABI
 * has no SHA-256 call, so the test uses the image builder as its oracle:
 * an image header carries the payload's SHA-256 at bytes 64..95. */
static const zxv_cap *g_cap;
static uint8_t *g_sk;
static int32_t sim_sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
    static uint8_t img[96 + 256 + 4627];
    size_t len;
    int32_t rc = zxv_hw_image_build(g_cap, ZXV_HW_IMG_PARTIAL, 1, NULL, 0, 0, 0, p, n, g_sk, img,
                                    sizeof img, &len);
    if (rc == ZXV_OK) memcpy(out, img + 64, 32);
    return rc;
}
static int32_t sim_readback(void *u, uint32_t s, uint8_t d[32])
{
    sim_t *x = u;
    return sim_sha256(x->slot[s], x->len[s], d);
}

static void test_regions(zxv_cap *cap, zxv_cap *nohw)
{
    static uint8_t pk[2592], sk[4896], pk2[2592], sk2[4896];
    uint8_t seed[32] = {7}, seed2[32] = {8};
    OK(zxv_mldsa87_keygen_seed(cap, seed, pk, sk));
    OK(zxv_mldsa87_keygen_seed(cap, seed2, pk2, sk2));
    g_cap = cap;
    g_sk = sk;

    zxv_hw_matrix *mx;
    OK(zxv_hw_matrix_create_cap(cap, &mx));
    OK(zxv_hw_function_add(cap, mx, 0x200, "inc"));
    OK(zxv_hw_resource_add(cap, mx, 1));
    OK(zxv_hw_resource_add(cap, mx, 2));
    static uint8_t vin[256], vinc[256], inc[256], wrong[256];
    for (int i = 0; i < 256; i++) {
        vin[i] = (uint8_t) i;
        vinc[i] = inc[i] = wrong[i] = (uint8_t) (i + 1);
    }
    wrong[77] = 0;
    OK(zxv_hw_vector_add(cap, mx, 0x200, 0, vin, 256, vinc, 256));
    static sim_t sim = {.active = -1};
    addk_t soft = {1, 0};
    uint32_t psoft, preg, pv;
    OK(zxv_hw_provider_add(cap, mx, 0x200, 1, ZXV_HW_KIND_SOFT, 50, addk, &soft, &psoft));
    OK(zxv_hw_provider_add(cap, mx, 0x200, 2, ZXV_HW_KIND_RECONF, 1, sim_call, &sim, &preg));
    int32_t lp;
    OK(zxv_hw_attest(cap, mx, psoft, &lp));

    zxv_hw_region *rg;
    IS(zxv_hw_region_create_cap(nohw, mx, preg, 1, 0xD0, NULL, sim_load, sim_readback, sim_activate,
                                &sim, 4, &rg),
       ZXV_E_CAP);
    IS(zxv_hw_region_create_cap(cap, mx, psoft, 1, 0xD0, NULL, sim_load, sim_readback, sim_activate,
                                &sim, 4, &rg),
       ZXV_E_ARG);
    OK(zxv_hw_region_create_cap(cap, mx, preg, 1, 0xD0, NULL, sim_load, sim_readback, sim_activate,
                                &sim, 4, &rg));
    OK(zxv_hw_region_trust_key(cap, rg, pk));

    static uint8_t img[96 + 256 + 4627];
    size_t len;
    IS(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 1, 1, inc, 256, sk, NULL, 0,
                          &len),
       ZXV_E_BUFFER);
    CHECK(len == sizeof img, "image length %zu", len);

    /* Wrong signer: refused, logged. */
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 1, 1, inc, 256, sk2, img,
                          sizeof img, &len));
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_VERIFY);
    /* Wrong hardware. */
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD1, NULL, 0x200, 1, 1, inc, 256, sk, img,
                          sizeof img, &len));
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_TARGET);

    /* The update: stage, trial, commit; the region now serves "inc". */
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 1, 1, inc, 256, sk, img,
                          sizeof img, &len));
    OK(zxv_hw_region_stage(cap, rg, img, len));
    IS(zxv_hw_region_commit(cap, rg), ZXV_E_STATE);
    OK(zxv_hw_region_trial(cap, rg, 5));
    OK(zxv_hw_bound(cap, mx, 0x200, &pv));
    CHECK(pv == preg, "the reconfigured region serves (%u)", pv);
    OK(zxv_hw_region_commit(cap, rg));
    int32_t st;
    uint32_t ver, floor, nq;
    OK(zxv_hw_region_info(cap, rg, &st, &ver, &floor, &nq));
    CHECK(st == ZXV_HW_REGION_ACTIVE && ver == 1 && floor == 1 && nq == 1, "info %d %u %u %u", st,
          ver, floor, nq);

    /* Replay: refused, logged. */
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_ROLLBACK);

    /* A signed image with a wrong gate: rolled back to v1, quarantined,
     * blocked even under a new version number. */
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 2, 1, wrong, 256, sk, img,
                          sizeof img, &len));
    OK(zxv_hw_region_stage(cap, rg, img, len));
    IS(zxv_hw_region_trial(cap, rg, 5), ZXV_E_QUARANTINED);
    OK(zxv_hw_region_info(cap, rg, &st, &ver, &floor, &nq));
    CHECK(st == ZXV_HW_REGION_ACTIVE && ver == 1 && nq == 3, "after rollback %d %u %u", st, ver, nq);
    OK(zxv_hw_bound(cap, mx, 0x200, &pv));
    CHECK(pv == preg, "v1 back in service");
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 3, 1, wrong, 256, sk, img,
                          sizeof img, &len));
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_BLOCKED);

    uint8_t head[32];
    uint32_t count;
    OK(zxv_hw_region_log(cap, rg, head, &count));
    CHECK(count == 3, "log has %u records", count);

    /* One more logged failure fills the log (4); the next one halts the
     * region and poisons the context: fail closed. */
    OK(zxv_hw_image_build(cap, ZXV_HW_IMG_PARTIAL, 0xD0, NULL, 0x200, 4, 1, inc, 256, sk2, img,
                          sizeof img, &len));
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_VERIFY);
    IS(zxv_hw_region_stage(cap, rg, img, len), ZXV_E_POISONED);
    IS(zxv_hw_call(cap, mx, 0x200, 0, vin, 4, vinc, 4, &len), ZXV_E_POISONED);

    zxv_hw_region_destroy(rg);
    zxv_hw_matrix_destroy(mx);
}

int main(void)
{
    uint32_t maj, min, pat, feat;
    OK(zxv_version(&maj, &min, &pat));
    CHECK(maj == 1 && min >= 4, "version %u.%u", maj, min);
    OK(zxv_features(&feat));
    CHECK(feat & ZXV_FEAT_HW, "ZXV_FEAT_HW missing");
    CHECK(!strcmp(zxv_status_name(ZXV_E_QUARANTINED), "ZXV_E_QUARANTINED"), "name");
    CHECK(zxv_status_class(ZXV_E_DISPUTE) == ZXV_CLASS_REFUSED, "class");
    CHECK(zxv_status_class(ZXV_E_TARGET) == ZXV_CLASS_USAGE, "class");

    test_profiles();

    zxv_ctx *ctx, *ctx2;
    zxv_cap *cap, *nohw, *cap2, *nohw2;
    OK(zxv_ctx_create(&ctx));
    OK(zxv_cap_create(ctx, 1, NULL, ZXV_CAP_ALL_1_4, &cap));
    OK(zxv_cap_attenuate(cap, ZXV_CAP_ALL, &nohw));
    IS(zxv_cap_create(ctx, 1, NULL, ZXV_CAP_ALL_1_4 + 1, &cap2), ZXV_E_ARG);
    test_matrix(cap, nohw);
    zxv_cap_destroy(nohw);
    zxv_cap_destroy(cap);
    zxv_ctx_destroy(ctx);

    OK(zxv_ctx_create(&ctx2));
    OK(zxv_cap_create(ctx2, 1, NULL, ZXV_CAP_ALL_1_4, &cap2));
    OK(zxv_cap_attenuate(cap2, ZXV_CAP_ALL, &nohw2));
    test_regions(cap2, nohw2);
    zxv_cap_destroy(nohw2);
    zxv_cap_destroy(cap2);
    zxv_ctx_destroy(ctx2);

    printf("test_zxv_abi14: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
