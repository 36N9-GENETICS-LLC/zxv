/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* hwr_mldsa.h — ML-DSA-87 sign/verify for reconfiguration images (hwr.h R1). */
#ifndef HWR_MLDSA_H
#define HWR_MLDSA_H

#include "hwr.h"

/* An hwr_verify_fn: ML-DSA-87 with the HWR_SIG_CONTEXT domain context. */
bool hwr_mldsa87_verify(void *ctx, uint16_t alg, const uint8_t *pk, uint32_t pk_len,
                        const uint8_t *msg, uint32_t msg_len, const uint8_t *sig, uint32_t sig_len);
/* Sign a 96-byte image header (tools and tests). sk: 4896 bytes, sig: 4627
 * bytes, rnd: 32 fresh bytes or NULL for deterministic signing. */
void hwr_mldsa87_sign(const uint8_t *sk, const uint8_t header[HWR_HEADER], const uint8_t *rnd,
                      uint8_t *sig);

#endif /* HWR_MLDSA_H */
