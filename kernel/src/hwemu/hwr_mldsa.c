/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* hwr_mldsa.c — the ML-DSA-87 binding of hwr_verify_fn (hosted / pqsec
 * builds). Kept apart from hwr.c so the policy stays crypto-free. */
#include "hwr_mldsa.h"
#include "../pqsec/pq_matrix_algs.h"

bool hwr_mldsa87_verify(void *ctx, uint16_t alg, const uint8_t *pk, uint32_t pk_len,
                        const uint8_t *msg, uint32_t msg_len, const uint8_t *sig, uint32_t sig_len)
{
    (void) ctx;
    if (alg != HWR_ALG_MLDSA87 || !pk || !msg || !sig || pk_len != PQM_MLDSA87_PK_BYTES ||
        sig_len != PQM_MLDSA87_SIG_BYTES)
        return false;
    return pqm_mldsa87_verify(pk, msg, msg_len, (const uint8_t *) HWR_SIG_CONTEXT,
                              sizeof HWR_SIG_CONTEXT - 1u, sig);
}

void hwr_mldsa87_sign(const uint8_t *sk, const uint8_t header[HWR_HEADER], const uint8_t *rnd,
                      uint8_t *sig)
{
    pqm_mldsa87_sign(sk, header, HWR_HEADER, (const uint8_t *) HWR_SIG_CONTEXT,
                     sizeof HWR_SIG_CONTEXT - 1u, rnd, sig);
}
