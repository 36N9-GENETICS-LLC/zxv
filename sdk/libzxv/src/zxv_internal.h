/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* zxv_internal.h — helpers shared by the libzxv adapter files. Not part of
 * the ABI: every symbol here has hidden visibility in the shared library
 * (-fvisibility=hidden) and the zxvi_ prefix in the static one. */
#ifndef ZXV_INTERNAL_H
#define ZXV_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zxv_api.h"

/* Record a failure detail (R7) and return `status`. */
int32_t zxvi_fail(int32_t status, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Clear the detail and return ZXV_OK. */
int32_t zxvi_ok(void);
/* R4 text out. */
int32_t zxvi_out_text_n(const char *s, size_t n, char *buf, size_t cap, size_t *out_len);
int32_t zxvi_out_text(const char *s, char *buf, size_t cap, size_t *out_len);
bool zxvi_utf8_valid(const char *s);
int32_t zxvi_check_str(const char *s, size_t max, const char *what, bool allow_empty);

/* Context access for the ABI 1.1 files. enter locks the context and fails
 * with ZXV_E_POISONED if it halted; leave unlocks. poison marks the context
 * halted (call it while entered) and returns ZXV_E_INVARIANT. */
int32_t zxvi_ctx_enter(zxv_ctx *ctx);
void zxvi_ctx_leave(zxv_ctx *ctx);
int32_t zxvi_ctx_poison(zxv_ctx *ctx, const char *why);
bool zxvi_ctx_is_poisoned(zxv_ctx *ctx);

/* Capability gate (zxv_caps.c): ZXV_OK, or ZXV_E_CAP / ZXV_E_POISONED with
 * the detail set. On ZXV_OK *out_ctx (if not NULL) is the cap's context. */
int32_t zxvi_cap_gate(const zxv_cap *cap, uint64_t needed, const char *what, zxv_ctx **out_ctx);

/* Ledger: an account's equity and currency, with the context entered. */
int32_t zxvi_ledger_equity(zxv_ctx *ctx, uint32_t account, const char *ccy, int64_t *out_equity);

/* ABI 1.1 feature bits built into this library (zxv_core.c). */
uint32_t zxvi_features_11(void);

/* Volatile zeroing used by every file that touches secrets. */
void zxvi_wipe(void *p, size_t n);

#endif /* ZXV_INTERNAL_H */
