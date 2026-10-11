/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* test_hwr.c — host tests for signed reconfiguration images (hwr.h R1-R6),
 * with real ML-DSA-87 signatures and a simulated two-slot region whose
 * "bitstream" is a 256-byte lookup table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwr.h"
#include "hwr_mldsa.h"
#include "../robin_debanks/sha256.h"
#include "../pqsec/pq_matrix_algs.h"

static int fails, checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

/* ---- the simulated region ---- */
typedef struct {
    uint8_t slot[2][256];
    uint32_t len[2];
    int active;      /* -1: blank */
    int corrupt;     /* 1: flip a bit on load (bad flash) */
    int refuse;      /* 1: activate fails */
    uint32_t loads;
} sim_t;

static int sim_load(void *d, uint32_t s, const uint8_t *p, uint32_t n)
{
    sim_t *x = d;
    if (s > 1 || n > 256) return -1;
    memcpy(x->slot[s], p, n);
    x->len[s] = n;
    if (x->corrupt) x->slot[s][0] ^= 1;
    x->loads++;
    return 0;
}
static int sim_readback(void *d, uint32_t s, uint8_t dg[32])
{
    sim_t *x = d;
    sha256(x->slot[s], x->len[s], dg);
    return 0;
}
static int sim_activate(void *d, uint32_t s)
{
    sim_t *x = d;
    if (x->refuse) return -1;
    x->active = (int) s;
    return 0;
}
static const hwr_ops_t SIM_OPS = {sim_load, sim_readback, sim_activate};

/* The RECONF provider: whatever table is active, applied byte by byte. */
static int sim_call(void *inst, hwm_t *m, uint32_t op, const uint8_t *in, uint32_t n, uint8_t *out,
                    uint32_t cap, uint32_t *out_len)
{
    (void) m;
    (void) op;
    sim_t *x = inst;
    if (x->active < 0 || n > cap) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = x->slot[x->active][in[i]];
    *out_len = n;
    return 0;
}
/* The software emulator of "inc": out = in + 1. */
static int soft_inc(void *inst, hwm_t *m, uint32_t op, const uint8_t *in, uint32_t n, uint8_t *out,
                    uint32_t cap, uint32_t *out_len)
{
    (void) inst;
    (void) m;
    (void) op;
    if (n > cap) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t) (in[i] + 1);
    *out_len = n;
    return 0;
}

/* ---- image building ---- */
static uint8_t pk[2592], sk[4896], pk2[2592], sk2[4896];
#define TCLASS 0x00D0F00Du

static uint32_t make(uint8_t *img, const uint8_t lut[256], uint32_t fn_id, uint32_t version,
                     uint32_t svn, const uint8_t *signer, const uint8_t tdig[32])
{
    hwr_image_t im;
    memset(&im, 0, sizeof im);
    im.alg = HWR_ALG_MLDSA87;
    im.kind = HWR_KIND_PARTIAL;
    im.target_class = TCLASS;
    im.fn_id = fn_id;
    im.version = version;
    im.svn = svn;
    im.payload_len = 256;
    if (tdig) memcpy(im.target_digest, tdig, 32);
    sha256(lut, 256, im.payload_sha256);
    hwr_build_header(&im, img);
    memcpy(img + HWR_HEADER, lut, 256);
    hwr_mldsa87_sign(signer, img, NULL, img + HWR_HEADER + 256);
    return HWR_HEADER + 256 + HWR_MLDSA87_SIG;
}

int main(void)
{
    uint8_t seed[32] = {1}, seed2[32] = {2};
    pqm_mldsa87_keygen(seed, pk, sk);
    pqm_mldsa87_keygen(seed2, pk2, sk2);

    static uint8_t inc[256], dec[256], wrong[256], xor5[256];
    for (int i = 0; i < 256; i++) {
        inc[i] = (uint8_t) (i + 1);
        dec[i] = (uint8_t) (i - 1);
        wrong[i] = (uint8_t) (i + 1);
        xor5[i] = (uint8_t) (i ^ 5);
    }
    wrong[200] = 0; /* a signed image with one wrong gate */

    /* Matrix: "inc" (soft emulator + the region) and "xor5" (region only). */
    static hwm_t m;
    hwm_init(&m);
    uint32_t finc, fxor, rcpu, rfab, psoft, preg;
    hwm_func_add(&m, 0x200, "inc", &finc);
    hwm_func_add(&m, 0x201, "xor5", &fxor);
    hwm_resource_add(&m, 1, &rcpu);
    hwm_resource_add(&m, 2, &rfab);
    static uint8_t vin[256], vinc[256], vxor[256];
    for (int i = 0; i < 256; i++) {
        vin[i] = (uint8_t) i;
        vinc[i] = (uint8_t) (i + 1);
        vxor[i] = (uint8_t) (i ^ 5);
    }
    hwm_vector_add(&m, finc, 0, vin, 256, vinc, 256);
    hwm_vector_add(&m, fxor, 0, vin, 256, vxor, 256);
    static sim_t sim = {.active = -1};
    hwm_provider_add(&m, finc, rcpu, HWM_KIND_SOFT, 50, soft_inc, 0, &psoft);
    hwm_provider_add(&m, finc, rfab, HWM_KIND_RECONF, 1, sim_call, &sim, &preg);
    CHECK(hwm_attest(&m, psoft, 0) == LPRES_STATE_TRUE);
    CHECK(hwm_attest(&m, preg, 0) == LPRES_STATE_FALSE); /* blank region */

    static lpx_qrec_t qrec[8];
    lpx_qlog_t q;
    lpx_qlog_init(&q, qrec, 8);
    static hwr_region_t r;
    uint8_t tdig[32];
    memset(tdig, 0xAB, 32);
    CHECK(hwr_region_init(&r, 9, TCLASS, tdig, HWR_ALG_MLDSA87, hwr_mldsa87_verify, 0, &SIM_OPS, &sim,
                          &q) == HWR_OK);
    CHECK(hwr_trust_key(&r, pk) == HWR_OK);
    CHECK(hwr_link(&r, &m, rfab, psoft) == HWR_E_ARG); /* not a RECONF provider */
    CHECK(hwr_link(&r, &m, rfab, preg) == HWR_OK);

    static uint8_t img[HWR_HEADER + 256 + HWR_MLDSA87_SIG];
    uint32_t n;
    uint32_t pv;

    /* R1: an untrusted signer is refused and logged; nothing loads. */
    n = make(img, inc, 0x200, 1, 1, sk2, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_E_VERIFY && q.n == 1 && sim.loads == 0);
    CHECK(q.rec[0].reason == HWR_Q_SIGNATURE && lpx_forbidden(q.rec[0].tag));
    /* Format: one byte short, or a tampered payload. */
    n = make(img, inc, 0x200, 1, 1, sk, NULL);
    CHECK(hwr_stage(&r, img, n - 1) == HWR_E_FORMAT);
    img[HWR_HEADER + 3] ^= 1;
    CHECK(hwr_stage(&r, img, n) == HWR_E_FORMAT);
    /* R2: another part of the class, or an unknown function. */
    uint8_t other[32];
    memset(other, 0xCD, 32);
    n = make(img, inc, 0x200, 1, 1, sk, other);
    CHECK(hwr_stage(&r, img, n) == HWR_E_TARGET);
    n = make(img, inc, 0x999, 1, 1, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_E_FUNCTION);

    /* The happy path: stage, trial (proved by vectors), commit. */
    n = make(img, inc, 0x200, 1, 1, sk, tdig);
    CHECK(hwr_stage(&r, img, n) == HWR_OK && r.state == HWR_ST_STAGED);
    CHECK(hwr_commit(&r) == HWR_E_STATE);
    CHECK(hwr_trial(&r, 3) == HWR_OK && r.state == HWR_ST_PROBATION);
    CHECK(m.pv[preg].attest == LPRES_STATE_TRUE);
    CHECK(hwm_bind(&m, finc, &pv) == HWM_OK && pv == preg); /* the hardware now serves */
    CHECK(hwr_commit(&r) == HWR_OK && r.state == HWR_ST_ACTIVE && r.floor == 1);

    /* R3: replaying the same version, or a lower svn, is refused and logged. */
    CHECK(hwr_stage(&r, img, n) == HWR_E_ROLLBACK && q.n == 2);
    n = make(img, inc, 0x200, 5, 0, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_E_ROLLBACK && q.n == 3);

    /* R5/R6: a validly signed image with a wrong gate fails conformance,
     * rolls back to v1, is quarantined and blocked. */
    n = make(img, wrong, 0x200, 2, 1, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_OK);
    CHECK(hwr_trial(&r, 3) == HWR_E_CONFORMANCE && r.state == HWR_ST_ACTIVE);
    CHECK(q.n == 4 && q.rec[3].reason == HWR_Q_CONFORMANCE && lpx_qlog_verify(&q));
    CHECK(m.pv[preg].attest == LPRES_STATE_TRUE && sim.active == (int) r.active);
    CHECK(hwm_bind(&m, finc, &pv) == HWM_OK && pv == preg);
    n = make(img, wrong, 0x200, 3, 1, sk, NULL); /* same bytes, new version */
    CHECK(hwr_stage(&r, img, n) == HWR_E_BLOCKED);

    /* R4: the flash corrupts what it stores: read-back catches it. */
    sim.corrupt = 1;
    n = make(img, dec, 0x200, 3, 1, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_E_READBACK && q.rec[q.n - 1].reason == HWR_Q_READBACK);
    sim.corrupt = 0;

    /* The region becomes a different accelerator: v5 is an xor5 table.
     * Left uncommitted, its probation expires and it rolls back. */
    n = make(img, xor5, 0x201, 5, 2, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_OK);
    CHECK(hwr_trial(&r, 2) == HWR_OK && m.pv[preg].fn == fxor);
    /* inc now comes from the emulator again, xor5 from the region. */
    CHECK(hwm_bind(&m, finc, &pv) == HWM_OK && pv == psoft);
    CHECK(hwm_bind(&m, fxor, &pv) == HWM_OK && pv == preg);
    CHECK(hwr_tick(&r) == HWR_OK);
    CHECK(hwr_tick(&r) == HWR_E_CONFORMANCE && r.state == HWR_ST_ACTIVE);
    CHECK(q.rec[q.n - 1].reason == HWR_Q_EXPIRED);
    CHECK(m.pv[preg].fn == finc && m.pv[preg].attest == LPRES_STATE_TRUE);

    /* Activation refused by the device: rolled back and quarantined. */
    sim.refuse = 1;
    n = make(img, xor5, 0x201, 6, 2, sk, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_OK);
    CHECK(hwr_trial(&r, 2) == HWR_E_ACTIVATE);
    /* With the device refusing even the fallback, the region drops to S0
     * and the emulator keeps "inc" running. */
    CHECK(r.state == HWR_ST_EMPTY && m.pv[preg].attest == LPRES_STATE_NEITHER);
    CHECK(hwm_bind(&m, finc, &pv) == HWM_OK && pv == psoft);
    sim.refuse = 0;

    /* A full quarantine log halts the region (fail closed). */
    CHECK(q.n == 7);
    n = make(img, inc, 0x200, 9, 3, sk2, NULL);
    CHECK(hwr_stage(&r, img, n) == HWR_E_VERIFY && q.n == 8);
    CHECK(hwr_stage(&r, img, n) == HWR_E_HALTED && r.state == HWR_ST_HALTED);
    CHECK(hwr_stage(&r, img, n) == HWR_E_HALTED);
    CHECK(lpx_qlog_verify(&q));

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
