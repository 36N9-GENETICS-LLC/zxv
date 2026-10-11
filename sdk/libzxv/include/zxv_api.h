/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* zxv_api.h — the stable, handle-based C ABI of the ZXV platform core.
 *
 * This header is the ONE place where every SDK (sdk/java, sdk/dotnet,
 * sdk/python, sdk/rust, sdk/go, sdk/typescript, sdk/swift, sdk/cpp, ...)
 * meets the freestanding kernel modules. The kernel headers behind it
 * (kernel/src/iso20022, finance, rational, lpres, rmag, phase_coord,
 * harmonic, swarm, pqsec, tensor, ...) are free to change; this surface is
 * not. sdk/docs/ABI.md explains every rule below with examples. Its rules:
 *
 *   R1  OPAQUE HANDLES. Callers only ever hold pointers to incomplete types
 *       (zxv_ctx, zxv_msg, zxv_netting). No kernel struct crosses the ABI,
 *       so no binding depends on a struct layout.
 *   R2  SCALARS ONLY. Every parameter is a fixed-width integer, a size_t,
 *       a NUL-terminated UTF-8 string, a caller-owned byte buffer, or a
 *       pointer to one of those (an out parameter). No floats: amounts are
 *       exact int64 minor units (cents, kobo, pesewas, ...).
 *   R3  STATUS CODES. Every function except the destroy functions and
 *       zxv_status_name returns int32_t: ZXV_OK (0) or a negative ZXV_E_*.
 *       Out parameters are written only on ZXV_OK, except *out_len on
 *       ZXV_E_BUFFER (see R4).
 *   R4  CALLER-OWNED BUFFERS, TWO-CALL PATTERN. A function that returns
 *       text takes (char *buf, size_t cap, size_t *out_len). *out_len always
 *       receives the length the full result needs, EXCLUDING the NUL. If buf
 *       is NULL or cap <= that length, the call returns ZXV_E_BUFFER and
 *       writes nothing to buf; allocate *out_len + 1 bytes and call again.
 *       On ZXV_OK the result is NUL-terminated.
 *   R5  UTF-8. All strings in and out are UTF-8. Input that is not valid
 *       UTF-8 is refused with ZXV_E_UTF8. ISO 20022 fields are XML-escaped
 *       by the serializer; callers pass plain text.
 *   R6  THREADING. A zxv_ctx is internally serialized by a mutex: any number
 *       of threads may call functions on the same context. A zxv_msg or
 *       zxv_netting handle is NOT thread-safe; use it from one thread at a
 *       time. Each handle must be destroyed exactly once, after every other
 *       call on it has returned. A child handle (msg, netting) must be
 *       destroyed before the zxv_ctx it was created from.
 *   R7  LAST-ERROR DETAIL. When a call fails, a human-readable reason is kept
 *       in thread-local storage; zxv_last_error copies it out. It is only
 *       diagnostic text: branch on the status code, never on the message.
 *   R8  VERSIONING. ZXV_ABI_VERSION_MAJOR changes only on a breaking change.
 *       New functions, new enum values and new ZXV_E_* codes are additive
 *       (MINOR). Bindings must treat an unknown negative status as an error.
 *   R9  FEATURES. Modules still being built in the kernel are present in the
 *       ABI as stubs that return ZXV_E_NOTIMPL. zxv_features reports which
 *       groups are live in THIS build, so a binding can light up features
 *       without recompiling.
 *
 * CURRENCIES. ISO 20022 wire messages carry ISO 4217 ALPHA codes ("NGN",
 * "GHS", "XOF") in Ccy attributes. The Vino rail numerics DEBIT 555 (NCR),
 * CREDIT 777 (NRE) and EQUITY 888 (PNS) are platform-internal: all three are
 * unassigned in ISO 4217, so this ABI refuses them, and any
 * private platform unit such as VFV, as a message currency (ZXV_E_CURRENCY).
 * They appear only in ledger rail balances and proprietary fields.
 *
 * HONEST LIMITS. This library serializes messages and keeps an in-process
 * ledger. It has no network transport: no PAPSS, SWIFT, CIPS or RTGS
 * connectivity is included, and nothing here is certified by any scheme.
 */
#ifndef ZXV_API_H
#define ZXV_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#    if defined(ZXV_BUILD_SHARED)
#        define ZXV_API __declspec(dllexport)
#    else
#        define ZXV_API __declspec(dllimport)
#    endif
#elif defined(__GNUC__) && defined(ZXV_BUILD_SHARED)
#    define ZXV_API __attribute__((visibility("default")))
#else
#    define ZXV_API
#endif

/* ===== Version ===== */
#define ZXV_ABI_VERSION_MAJOR 1
#define ZXV_ABI_VERSION_MINOR 1
#define ZXV_ABI_VERSION_PATCH 0

/* ===== Status codes (stable; never renumbered) ===== */
#define ZXV_OK            0
#define ZXV_E_ARG         (-1)  /* NULL handle/pointer or invalid argument value  */
#define ZXV_E_BUFFER      (-2)  /* output buffer too small; *out_len = needed     */
#define ZXV_E_NOTIMPL     (-3)  /* feature not built into this library yet        */
#define ZXV_E_NOT_FOUND   (-4)  /* unknown account, card, currency or index       */
#define ZXV_E_CAPACITY    (-5)  /* a fixed-size table is full                     */
#define ZXV_E_FUNDS       (-6)  /* holder account would go below zero             */
#define ZXV_E_CURRENCY    (-7)  /* unknown, disabled, non-ISO or mismatched ccy   */
#define ZXV_E_RANGE       (-8)  /* amount <= 0 or beyond the exact range          */
#define ZXV_E_STATE       (-9)  /* operation not valid in the current state       */
#define ZXV_E_INVARIANT   (-10) /* an internal accounting invariant failed        */
#define ZXV_E_UTF8        (-11) /* input string is not valid UTF-8                */
#define ZXV_E_TOO_LONG    (-12) /* input string exceeds the field's maximum       */
#define ZXV_E_DUPLICATE   (-13) /* reference/id already used                      */
#define ZXV_E_NOMEM       (-14) /* allocation failed                              */
#define ZXV_E_DECLINED    (-15) /* card authorization declined (not a fault)      */
#define ZXV_E_FIELD       (-16) /* field not valid for this message kind / unset  */
/* ABI 1.1 */
#define ZXV_E_CAP         (-17) /* the zxv_cap handle lacks the capability, is    */
                                /* revoked, or its attestation is not TRUE        */
#define ZXV_E_POISONED    (-18) /* the context halted after a broken invariant;   */
                                /* terminal: every later call fails the same way  */
#define ZXV_E_BUDGET      (-19) /* an rmag or swarm budget is exhausted (denied)  */
#define ZXV_E_RMAG_INVALID (-20) /* RMAG_RESULT_INVALID: malformed or overflowing */
                                /* rational amount; never retry the same value    */
#define ZXV_E_VERIFY      (-21) /* signature / tag did not verify (not a fault)   */
#define ZXV_E_FORMAT      (-22) /* malformed input (number text, GGUF, frame)     */
#define ZXV_E_IO          (-23) /* a file could not be opened or read             */
#define ZXV_E_FLOAT       (-24) /* NaN or infinity refused at the float boundary  */
#define ZXV_E_INTERNAL    (-99) /* unexpected kernel status; see zxv_last_error   */

/* Status classes (zxv_status_class). Every SDK maps them the same way:
 *   USAGE   a caller bug: fix the arguments; retrying the same call fails again
 *   REFUSED a well-formed request the platform declined (funds, capability,
 *           budget, signature): handle it; a retry may succeed after the
 *           situation changes (a budget is replenished, a capability granted)
 *   FATAL   ZXV_E_INVARIANT / ZXV_E_POISONED: the context is halted and rolled
 *           back; never retry; SDKs raise their terminal error type
 *   SYSTEM  out of memory, I/O, internal: environmental */
#define ZXV_CLASS_OK      0
#define ZXV_CLASS_USAGE   1
#define ZXV_CLASS_REFUSED 2
#define ZXV_CLASS_FATAL   3
#define ZXV_CLASS_SYSTEM  4

/* ===== Feature bits (zxv_features) ===== */
#define ZXV_FEAT_LEDGER      0x00000001u /* triple-ledger accounts/postings     */
#define ZXV_FEAT_MX_PACS008  0x00000002u
#define ZXV_FEAT_MX_CAMT053  0x00000004u
#define ZXV_FEAT_ISO4217     0x00000008u /* full ISO 4217 table lookup          */
#define ZXV_FEAT_MX_PACS002  0x00000010u
#define ZXV_FEAT_MX_PACS004  0x00000020u
#define ZXV_FEAT_MX_PACS009  0x00000040u
#define ZXV_FEAT_MX_CAMT056  0x00000080u
#define ZXV_FEAT_MX_CAMT029  0x00000100u
#define ZXV_FEAT_NETTING     0x00000200u /* multilateral netting cycles         */
#define ZXV_FEAT_CARDS       0x00000400u /* card issue/reissue/authorize        */
#define ZXV_FEAT_CARD_CHECK  0x00000800u /* PAN check digits (Luhn/Damm/Verh.)  */
#define ZXV_FEAT_VSS         0x00001000u /* VSS conformance checks              */
/* ABI 1.1 */
#define ZXV_FEAT_RATIONAL    0x00002000u /* exact rationals (rational.h rat_t)  */
#define ZXV_FEAT_LPRES       0x00004000u /* four-valued logic + attestations    */
#define ZXV_FEAT_CAPS        0x00008000u /* capability handles (zxv_cap)        */
#define ZXV_FEAT_RMAG        0x00010000u /* rational resource budgets (K2)      */
#define ZXV_FEAT_PHASE       0x00020000u /* phase coordinator ticks (K6)        */
#define ZXV_FEAT_WIRE        0x00040000u /* UBH-168 harmonic wire frames        */
#define ZXV_FEAT_SWARM       0x00080000u /* Fibonacci + emotional token economy */
#define ZXV_FEAT_HASH        0x00100000u /* SHA3-256, SHAKE256, Keccak-256      */
#define ZXV_FEAT_MLKEM1024   0x00200000u /* FIPS 203 ML-KEM-1024                */
#define ZXV_FEAT_MLDSA87     0x00400000u /* FIPS 204 ML-DSA-87                  */
#define ZXV_FEAT_ED25519     0x00800000u /* RFC 8032 Ed25519                    */
#define ZXV_FEAT_TENSOR      0x01000000u /* integer GGUF inference engine       */
#define ZXV_FEAT_LORA        0x02000000u /* LoRA adapters (NOT built yet)       */
#define ZXV_FEAT_EVM         0x04000000u /* EVM state-channel encoding          */

/* ===== Opaque handles ===== */
typedef struct zxv_ctx zxv_ctx;         /* one platform instance (ledger, config) */
typedef struct zxv_msg zxv_msg;         /* one ISO 20022 message under construction */
typedef struct zxv_netting zxv_netting; /* one netting cycle                     */

/* ===== Library ===== */

/* ABI version of the loaded library. Any out pointer may be NULL. */
ZXV_API int32_t zxv_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

/* Bitmask of ZXV_FEAT_* live in this build. */
ZXV_API int32_t zxv_features(uint32_t *out_mask);

/* Static, never-NULL symbolic name for a status ("ZXV_E_FUNDS"). Not owned
 * by the caller; never free it. Unknown codes give "ZXV_E_UNKNOWN". */
ZXV_API const char *zxv_status_name(int32_t status);

/* ZXV_CLASS_* of a status (ABI 1.1). Unknown negative codes are SYSTEM. */
ZXV_API int32_t zxv_status_class(int32_t status);

/* Copy the calling thread's last error detail (R7). "" if none. */
ZXV_API int32_t zxv_last_error(char *buf, size_t cap, size_t *out_len);

/* ===== Context ===== */

/* Create a platform context: an empty ledger with no currencies enabled. */
ZXV_API int32_t zxv_ctx_create(zxv_ctx **out_ctx);

/* Destroy a context. NULL is ignored. */
ZXV_API void zxv_ctx_destroy(zxv_ctx *ctx);

/* 1 if the context halted after a broken invariant (ABI 1.1). A poisoned
 * context refuses every further call with ZXV_E_POISONED; it can only be
 * destroyed. This is the C form of "fail closed": a host may not retry a
 * mathematically corrupted transaction on the same state. */
ZXV_API int32_t zxv_ctx_is_poisoned(zxv_ctx *ctx, int32_t *out_poisoned);

/* Halt a context on purpose (an operator kill switch; also how SDK tests
 * exercise the fatal path). Afterwards it behaves exactly like a context
 * that broke an invariant. reason <= 150 bytes, may be NULL. */
ZXV_API int32_t zxv_ctx_halt(zxv_ctx *ctx, const char *reason);

/* ===== ISO 4217 reference data (process-wide, read-only) ===== */

#define ZXV_CCYF_FUND     0x01u /* fund code (e.g. CLF, USN)                    */
#define ZXV_CCYF_MINOR_NA 0x02u /* minor unit N.A. (metals, XDR, XTS, XXX)      */
#define ZXV_CCYF_NOCTRY   0x04u /* not the currency of any ISO 3166 country     */
#define ZXV_CCYF_PAYABLE  0x08u /* usable for a payment amount                  */
#define ZXV_CCYF_PRIVATE  0x10u /* platform-private unit registered in a ctx;   */
                                /* never valid in an ISO 20022 Ccy attribute    */
#define ZXV_MINOR_NA 255u

/* Look up an ISO 4217 currency by alpha-3 ("NGN"). Out pointers may be NULL.
 * ZXV_E_NOT_FOUND for anything not in ISO 4217 (including VFV). */
ZXV_API int32_t zxv_iso4217_by_alpha(const char *alpha, uint16_t *out_numeric,
                                     uint8_t *out_minor_units, uint32_t *out_flags);

/* Look up by ISO 4217 numeric (566 -> "NGN"). 555/777/888 -> ZXV_E_NOT_FOUND. */
ZXV_API int32_t zxv_iso4217_by_numeric(uint16_t numeric, char *alpha_buf, size_t cap,
                                       size_t *out_len, uint8_t *out_minor_units,
                                       uint32_t *out_flags);

/* English name of an ISO 4217 currency ("Naira"). */
ZXV_API int32_t zxv_iso4217_name(const char *alpha, char *buf, size_t cap, size_t *out_len);

/* Number of entries in the ISO 4217 table, and enumeration by index. */
ZXV_API int32_t zxv_iso4217_count(uint32_t *out_count);
ZXV_API int32_t zxv_iso4217_at(uint32_t index, char *alpha_buf, size_t cap, size_t *out_len);

/* Publication date of the ISO 4217 list compiled in ("2026-01-01"). */
ZXV_API int32_t zxv_iso4217_published(char *buf, size_t cap, size_t *out_len);

/* Interop caveat for a numeric code (e.g. 777 = Vino CREDIT rail), "" if none. */
ZXV_API int32_t zxv_ccy_caveat(uint16_t numeric, char *buf, size_t cap, size_t *out_len);

/* True (1) if ISO 3166 alpha-2 country is an African Union member. */
ZXV_API int32_t zxv_au_is_member(const char *country_a2, int32_t *out_is_member);

/* ===== Vino rails ===== */
typedef enum {
    ZXV_RAIL_DEBIT = 0,  /* 555 NCR: asset / backing  */
    ZXV_RAIL_CREDIT = 1, /* 777 NRE: claim / liability */
    ZXV_RAIL_EQUITY = 2  /* 888 PNS: debit - credit   */
} zxv_rail;

/* Rail -> internal numeric (555/777/888). Proprietary fields only. */
ZXV_API int32_t zxv_rail_numeric(int32_t rail, uint16_t *out_numeric);

/* ===== Currency configuration (per context) ===== */

/* Enable an ISO 4217 currency for ledger accounts in this context. Minor
 * units come from the ISO 4217 table. ZXV_E_CURRENCY if not payable. */
ZXV_API int32_t zxv_ccy_enable(zxv_ctx *ctx, const char *alpha);

/* Enable an ISO 4217 currency with operator-supplied minor units: only for
 * builds without the ISO 4217 table, or to assert the expected value (a
 * mismatch with the table is ZXV_E_CURRENCY). */
ZXV_API int32_t zxv_ccy_enable_with_minor(zxv_ctx *ctx, const char *alpha, uint8_t minor_units);

/* Register a platform-private unit (e.g. "VFV", 2 minor units). It can be
 * held in ledger accounts but is flagged ZXV_CCYF_PRIVATE and refused as an
 * ISO 20022 currency. `code` is 3..8 upper-case letters/digits and must not
 * collide with an ISO 4217 alpha. */
ZXV_API int32_t zxv_ccy_register_private(zxv_ctx *ctx, const char *code, uint8_t minor_units);

/* Is `code` enabled in this context? Gives its minor units and flags. */
ZXV_API int32_t zxv_ccy_get(zxv_ctx *ctx, const char *code, uint8_t *out_minor_units,
                            uint32_t *out_flags);

/* Enumerate enabled currencies. */
ZXV_API int32_t zxv_ccy_enabled_count(zxv_ctx *ctx, uint32_t *out_count);
ZXV_API int32_t zxv_ccy_enabled_at(zxv_ctx *ctx, uint32_t index, char *buf, size_t cap,
                                   size_t *out_len);

/* ===== Ledger (finance/triple_ledger + exact shadow) ===== */

typedef enum {
    ZXV_ACCT_HOLDER = 0, /* may never go below zero (no debt)                */
    ZXV_ACCT_ISSUER = 1  /* may carry a CREDIT (777) balance: issuance/nostro */
} zxv_account_kind;

/* Largest amount accepted in one posting and largest |balance| (2^53 - 1),
 * so that every value is exact in every kernel number representation. */
#define ZXV_AMOUNT_MAX ((int64_t) 9007199254740991LL)

/* Open an account in an enabled currency. `name` <= 63 bytes. */
ZXV_API int32_t zxv_ledger_open_account(zxv_ctx *ctx, const char *name, const char *ccy,
                                        int32_t kind, uint32_t *out_account_id);

/* Post a balanced transfer of `amount_minor` (> 0) from `from_account` to
 * `to_account`, both in `ccy`. Posts the financial, provenance and
 * externality axes of the triple ledger atomically: it is fully validated
 * (funds, currency, range, uniqueness) and every resource is reserved before
 * any balance is written. `reference` (<= 35 bytes, e.g. the EndToEndId)
 * must be unique in the context. *out_entry_id receives the platform entry
 * id (monotonic from 1, shared by both sides). Journals are unbounded: the
 * adapter chains fixed-size kernel triple-ledger segments transparently. */
ZXV_API int32_t zxv_ledger_post(zxv_ctx *ctx, uint32_t from_account, uint32_t to_account,
                                int64_t amount_minor, const char *ccy, const char *reference,
                                uint64_t *out_entry_id);

/* Rail balances of an account in minor units: DEBIT 555 (sum received),
 * CREDIT 777 (sum sent), EQUITY 888 (= debit - credit, the spendable
 * position). Any out pointer may be NULL. */
ZXV_API int32_t zxv_ledger_balance(zxv_ctx *ctx, uint32_t account, int64_t *out_debit_minor,
                                   int64_t *out_credit_minor, int64_t *out_equity_minor);

/* Account metadata. */
ZXV_API int32_t zxv_ledger_account_ccy(zxv_ctx *ctx, uint32_t account, char *buf, size_t cap,
                                       size_t *out_len);
ZXV_API int32_t zxv_ledger_account_name(zxv_ctx *ctx, uint32_t account, char *buf, size_t cap,
                                        size_t *out_len);
ZXV_API int32_t zxv_ledger_account_kind(zxv_ctx *ctx, uint32_t account, int32_t *out_kind);
ZXV_API int32_t zxv_ledger_account_count(zxv_ctx *ctx, uint32_t *out_count);

/* Journal of one account, oldest first. Each line: entry id, signed amount
 * (+ received / - sent), counterparty account, reference. */
ZXV_API int32_t zxv_ledger_entry_count(zxv_ctx *ctx, uint32_t account, uint32_t *out_count);
ZXV_API int32_t zxv_ledger_entry_at(zxv_ctx *ctx, uint32_t account, uint32_t index,
                                    uint64_t *out_entry_id, int64_t *out_signed_minor,
                                    uint32_t *out_counterparty, char *ref_buf, size_t ref_cap,
                                    size_t *out_ref_len);

/* Verify the ledger: per currency sum(debit) == sum(credit), every holder
 * >= 0, exact shadow == triple-ledger financial axis. ZXV_E_INVARIANT with
 * detail in zxv_last_error on failure. */
ZXV_API int32_t zxv_ledger_check(zxv_ctx *ctx);

/* ===== ISO 20022 messages ===== */

typedef enum {
    ZXV_MX_PACS008 = 1, /* FIToFICstmrCdtTrf      pacs.008.001.09 */
    ZXV_MX_CAMT053 = 2, /* BkToCstmrStmt          camt.053.001.08 */
    ZXV_MX_PACS002 = 3, /* FIToFIPmtStsRpt        (cbank, pending) */
    ZXV_MX_PACS004 = 4, /* PmtRtr                 (cbank, pending) */
    ZXV_MX_PACS009 = 5, /* FICdtTrf               (cbank, pending) */
    ZXV_MX_CAMT056 = 6, /* FIToFIPmtCxlReq        (cbank, pending) */
    ZXV_MX_CAMT029 = 7  /* RsltnOfInvstgtn        (cbank, pending) */
} zxv_mx_kind;

/* Text fields. Each kind accepts a subset; others give ZXV_E_FIELD. */
typedef enum {
    ZXV_FLD_MSG_ID = 1,        /* GrpHdr/MsgId                 <= 35  all     */
    ZXV_FLD_CRE_DT_TM = 2,     /* GrpHdr/CreDtTm (ISO 8601)    <= 32  all     */
    ZXV_FLD_DEBTOR_NAME = 3,   /* Dbtr/Nm                      <= 70  pacs008 */
    ZXV_FLD_DEBTOR_ACCT = 4,   /* DbtrAcct/Id                  <= 35  pacs008 */
    ZXV_FLD_CREDITOR_NAME = 5, /* Cdtr/Nm                      <= 70  pacs008 */
    ZXV_FLD_CREDITOR_ACCT = 6, /* CdtrAcct/Id                  <= 35  pacs008 */
    ZXV_FLD_END_TO_END_ID = 7, /* PmtId/EndToEndId             <= 35  pacs008 */
    ZXV_FLD_CCY = 8,           /* ISO 4217 alpha-3             == 3   all     */
    ZXV_FLD_ACCT_ID = 9        /* Stmt/Acct/Id/Othr/Id         <= 35  camt053 */
} zxv_mx_field;

/* Amount fields (exact minor units + decimal places). */
typedef enum {
    ZXV_AMT_SETTLEMENT = 1, /* pacs008 IntrBkSttlmAmt / InstdAmt */
    ZXV_AMT_OPENING = 2,    /* camt053 OPBD                      */
    ZXV_AMT_CLOSING = 3     /* camt053 CLBD                      */
} zxv_mx_amount;

typedef enum { ZXV_CRDT = 0, ZXV_DBIT = 1 } zxv_cdtdbt;
typedef enum { ZXV_STS_BOOK = 0, ZXV_STS_PDNG = 1, ZXV_STS_INFO = 2 } zxv_entry_status;

/* Create a message of `kind`. The context validates currencies (enabled is
 * not required, but ISO 4217 membership and minor units are checked).
 * ZXV_E_NOTIMPL for a kind whose kernel builder is not available. */
ZXV_API int32_t zxv_msg_create(zxv_ctx *ctx, int32_t kind, zxv_msg **out_msg);
ZXV_API void zxv_msg_destroy(zxv_msg *msg);

ZXV_API int32_t zxv_msg_set_text(zxv_msg *msg, int32_t field, const char *utf8);

/* `frac_digits` must equal the currency's ISO 4217 minor units once the
 * currency is set (checked again at render). Negative balances are allowed
 * for camt053 opening/closing (rendered with DBIT). */
ZXV_API int32_t zxv_msg_set_amount(zxv_msg *msg, int32_t field, int64_t units,
                                   uint8_t frac_digits);

/* Number of transactions (pacs008 GrpHdr/NbOfTxs). Default 1. */
ZXV_API int32_t zxv_msg_set_tx_count(zxv_msg *msg, uint32_t nb_of_txs);

/* Append one camt053 statement entry (non-negative magnitude + direction). */
ZXV_API int32_t zxv_msg_add_entry(zxv_msg *msg, int64_t units, uint8_t frac_digits,
                                  int32_t cdt_dbt, int32_t status);

/* Serialize to XML (R4 two-call pattern). Validates that every mandatory
 * field is set (ZXV_E_FIELD) and the currency (ZXV_E_CURRENCY). */
ZXV_API int32_t zxv_msg_render(zxv_msg *msg, char *buf, size_t cap, size_t *out_len);

/* ===== Netting cycles (kernel/src/cbank — pending) ===== */

/* Open a multilateral netting cycle in one settlement currency. */
ZXV_API int32_t zxv_netting_open(zxv_ctx *ctx, const char *cycle_id, const char *ccy,
                                 zxv_netting **out_cycle);
ZXV_API void zxv_netting_destroy(zxv_netting *cycle);

/* Add a participant (e.g. a BIC or PAPSS participant code). */
ZXV_API int32_t zxv_netting_add_participant(zxv_netting *cycle, const char *participant);

/* Submit one obligation debtor -> creditor, idempotent on `payment_id`. */
ZXV_API int32_t zxv_netting_submit(zxv_netting *cycle, const char *payment_id,
                                   const char *debtor, const char *creditor,
                                   int64_t amount_minor);

/* Close the cycle and compute net positions. */
ZXV_API int32_t zxv_netting_close(zxv_netting *cycle);

/* Net positions after close: + receives, - pays. Sum is always zero. */
ZXV_API int32_t zxv_netting_position_count(zxv_netting *cycle, uint32_t *out_count);
ZXV_API int32_t zxv_netting_position_at(zxv_netting *cycle, uint32_t index, char *buf,
                                        size_t cap, size_t *out_len, int64_t *out_net_minor);

/* ===== Cards (kernel/src/cardnet — pending except check digits) ===== */

typedef enum {
    ZXV_NET_PHOENIX = 1,     /* Damm check digit     */
    ZXV_NET_DRAGON = 2,      /* Luhn check digit     */
    ZXV_NET_THUNDERBIRD = 3  /* Verhoeff check digit */
} zxv_card_network;

/* Validate the check digit of a decimal PAN for a network (1 = valid). */
ZXV_API int32_t zxv_card_check_digit(int32_t network, const char *pan, int32_t *out_valid);

/* Issue a charge card bound to a ledger account. Writes a masked PAN. */
ZXV_API int32_t zxv_card_issue(zxv_ctx *ctx, int32_t network, uint32_t account,
                               int64_t limit_minor, uint64_t *out_card_id, char *masked_pan,
                               size_t cap, size_t *out_len);

/* Reissue (lost/expired): the old card is blocked, a new id is returned. */
ZXV_API int32_t zxv_card_reissue(zxv_ctx *ctx, uint64_t card_id, uint64_t *out_new_card_id);

/* Authorize an amount. ZXV_OK with an approval code, or ZXV_E_DECLINED with
 * an ISO 8583 response code in *out_response (e.g. 51 insufficient funds). */
ZXV_API int32_t zxv_card_authorize(zxv_ctx *ctx, uint64_t card_id, int64_t amount_minor,
                                   const char *ccy, int32_t *out_response, char *approval_buf,
                                   size_t cap, size_t *out_len);

/* ===== VSS conformance (kernel/src/cbank — pending) ===== */

typedef enum {
    ZXV_VSS_C1_BALANCE = 1,   /* every cycle nets to zero                 */
    ZXV_VSS_C2_RECEIPTS = 2,  /* every payment has a receipt              */
    ZXV_VSS_C3_ROUNDTRIP = 3, /* MX build -> parse -> build is identical  */
    ZXV_VSS_C4_BACKING = 4    /* settlement backed by funded positions    */
} zxv_vss_check;

typedef enum { ZXV_VSS_NONE = 0, ZXV_VSS_BRONZE = 1, ZXV_VSS_SILVER = 2, ZXV_VSS_GOLD = 3 } zxv_vss_level;

/* Run one check over the context. *out_passed = 1/0; detail text (R4). */
ZXV_API int32_t zxv_vss_run(zxv_ctx *ctx, int32_t check, int32_t *out_passed, char *detail,
                            size_t cap, size_t *out_len);

/* Overall conformance level (zxv_vss_level). */
ZXV_API int32_t zxv_vss_level_get(zxv_ctx *ctx, int32_t *out_level);

/* ===================================================================== */
/* ABI 1.1: the AI swarm, exactness, logic, security and engine groups.  */
/*                                                                       */
/* Rules R1-R9 above hold unchanged. Two more:                           */
/*   R10 CAPABILITIES. Every call that touches state, keys or the engine */
/*       takes a `const zxv_cap *` as its first argument and verifies it */
/*       on EVERY call (sdk/docs/ABI.md, "Capabilities"). Pure functions */
/*       (rationals, logic tables, hashes, wire framing) take none.      */
/*   R11 NO FLOATS. No parameter is float or double. Q16 values are      */
/*       int32 fixed point (value = q / 65536). A host float can enter   */
/*       only through zxv_rat_from_ieee754, which takes the raw IEEE-754 */
/*       BITS and returns the exact rational they denote.                */
/* ===================================================================== */

/* ===== Exact rationals (kernel/src/rational: rat_t, the orbital_compat
 * IR numeric normal form) =====
 * A rational crosses the ABI as two int64: numerator and denominator.
 * Results are always reduced with den > 0. Overflow is detected, never
 * silent: ZXV_E_RANGE. A zero denominator is ZXV_E_ARG. INT64_MIN is
 * refused as a numerator or denominator (its negation is not an int64). */

ZXV_API int32_t zxv_rat_normalize(int64_t num, int64_t den, int64_t *out_num, int64_t *out_den);
ZXV_API int32_t zxv_rat_add(int64_t an, int64_t ad, int64_t bn, int64_t bd, int64_t *out_num,
                            int64_t *out_den);
ZXV_API int32_t zxv_rat_sub(int64_t an, int64_t ad, int64_t bn, int64_t bd, int64_t *out_num,
                            int64_t *out_den);
ZXV_API int32_t zxv_rat_mul(int64_t an, int64_t ad, int64_t bn, int64_t bd, int64_t *out_num,
                            int64_t *out_den);
/* Division by zero (bn == 0) is ZXV_E_ARG. */
ZXV_API int32_t zxv_rat_div(int64_t an, int64_t ad, int64_t bn, int64_t bd, int64_t *out_num,
                            int64_t *out_den);
/* *out_cmp = -1, 0 or +1. Exact; no epsilon. */
ZXV_API int32_t zxv_rat_cmp(int64_t an, int64_t ad, int64_t bn, int64_t bd, int32_t *out_cmp);

/* Parse "12.75", "-0.1", "3/4" or "42" exactly. ZXV_E_FORMAT on anything
 * else (exponents, spaces, "NaN"), ZXV_E_RANGE if it does not fit. */
ZXV_API int32_t zxv_rat_parse(const char *text, int64_t *out_num, int64_t *out_den);

/* Render exactly (R4): a terminating decimal when one exists ("12.75"),
 * otherwise "n/d" ("1/3"). Never rounds. */
ZXV_API int32_t zxv_rat_format(int64_t num, int64_t den, char *buf, size_t cap, size_t *out_len);

/* THE FLOAT BOUNDARY. `bits` is the raw IEEE-754 binary64 encoding of a
 * host double (Java Double.doubleToRawLongBits, C# BitConverter.
 * DoubleToInt64Bits, Python struct.pack('<d'), ...). Every finite double
 * IS an exact rational m * 2^e; this returns that rational, so 0.1 maps
 * to 3602879701896397/36028797018963968, never to 1/10: the drift is made
 * visible instead of hidden. NaN and infinities: ZXV_E_FLOAT. A value whose
 * exact form needs more than 63 bits (e.g. 2^-80): ZXV_E_RANGE.
 * SDKs never call this implicitly; it backs an explicit, named API. */
ZXV_API int32_t zxv_rat_from_ieee754(uint64_t bits, int64_t *out_num, int64_t *out_den);

/* Round a rational to Q16.16 fixed point (round half away from zero).
 * ZXV_E_RANGE outside int32. Used for engine parameters (temperature). */
ZXV_API int32_t zxv_rat_to_q16(int64_t num, int64_t den, int32_t *out_q16);

/* ===== Paraconsistent four-valued logic (kernel/src/lpres, K3) =====
 * Values are the kernel's lpres_state_t, fixed forever. "Both" is a held
 * contradiction, not an error; SDKs must never collapse it to a boolean. */
#define ZXV_LP_NEITHER 0 /* S0: unknown, unattested, pending */
#define ZXV_LP_TRUE    1 /* S+: attested present and valid   */
#define ZXV_LP_FALSE   2 /* S-: attested absent or invalid   */
#define ZXV_LP_BOTH    3 /* contradictory evidence, held      */

ZXV_API int32_t zxv_lp_not(int32_t a, int32_t *out);
ZXV_API int32_t zxv_lp_and(int32_t a, int32_t b, int32_t *out);
ZXV_API int32_t zxv_lp_or(int32_t a, int32_t b, int32_t *out);
/* From evidence counts: pos > 0 and neg > 0 -> BOTH, only pos -> TRUE,
 * only neg -> FALSE, none -> NEITHER. */
ZXV_API int32_t zxv_lp_from_evidence(uint32_t positive, uint32_t negative, int32_t *out);
/* Static name ("BOTH"); "INVALID" for an unknown value. */
ZXV_API const char *zxv_lp_name(int32_t value);

/* ===== Capabilities (the sb_lang_context_t of kernel/src/sdk_bridge) =====
 * A zxv_cap carries what the kernel's language context carries: a process
 * id, a 21-octet (168-bit) identity, a capability set and an attestation
 * state. Gated calls verify, every time: the handle is live (not revoked,
 * nor any ancestor), the needed bit is set, and the attestation is TRUE.
 * A cap can only be narrowed (zxv_cap_attenuate), never widened.
 * HONEST LIMIT: inside one process this is least-privilege discipline,
 * not a sandbox; code in the same address space can create its own cap.
 * The process boundary (zxvd over IPC) is where it becomes enforcement. */
typedef struct zxv_cap zxv_cap;

#define ZXV_CAP_LEDGER_READ   (UINT64_C(1) << 0)  /* read balances, journals    */
#define ZXV_CAP_LEDGER_POST   (UINT64_C(1) << 1)  /* SB_CAP_CAPITAL_TRANSFER     */
#define ZXV_CAP_CRYPTO_SIGN   (UINT64_C(1) << 2)  /* SB_CAP_CRYPTO_SIGN          */
#define ZXV_CAP_CRYPTO_KEM    (UINT64_C(1) << 3)  /* SB_CAP_CRYPTO_KEM           */
#define ZXV_CAP_AI_INFERENCE  (UINT64_C(1) << 4)  /* SB_CAP_AI_INFERENCE         */
#define ZXV_CAP_BUDGET        (UINT64_C(1) << 5)  /* SB_CAP_COMPUTE_SCHEDULE     */
#define ZXV_CAP_PHASE         (UINT64_C(1) << 6)  /* admit / defer / advance     */
#define ZXV_CAP_ATTEST        (UINT64_C(1) << 7)  /* SB_CAP_IDENTITY_CREDENTIAL  */
#define ZXV_CAP_SWARM         (UINT64_C(1) << 8)  /* swarm economy               */
#define ZXV_CAP_EVM           (UINT64_C(1) << 9)  /* state-channel encoding      */
#define ZXV_CAP_ALL           ((UINT64_C(1) << 10) - 1)

#define ZXV_IDENTITY_BYTES 21

/* Create a root capability for a context. identity may be NULL (zeros).
 * Unknown bits in `caps` are ZXV_E_ARG. Attestation starts TRUE. */
ZXV_API int32_t zxv_cap_create(zxv_ctx *ctx, uint32_t process_id, const uint8_t *identity,
                               uint64_t caps, zxv_cap **out_cap);
/* A child holding (parent caps & keep). The parent must outlive it. */
ZXV_API int32_t zxv_cap_attenuate(const zxv_cap *parent, uint64_t keep, zxv_cap **out_cap);
/* Permanently revoke this cap and, through it, every descendant. */
ZXV_API int32_t zxv_cap_revoke(zxv_cap *cap);
/* Set the attestation (ZXV_LP_*). Anything but TRUE blocks gated calls;
 * BOTH reports "contradictory attestation". */
ZXV_API int32_t zxv_cap_set_attestation(zxv_cap *cap, int32_t lp_state);
ZXV_API int32_t zxv_cap_info(const zxv_cap *cap, uint64_t *out_caps, uint32_t *out_process_id,
                             int32_t *out_attestation, int32_t *out_revoked);
/* ZXV_OK if `cap` could make a call needing `needed`, else ZXV_E_CAP. */
ZXV_API int32_t zxv_cap_check(const zxv_cap *cap, uint64_t needed);
ZXV_API void zxv_cap_destroy(zxv_cap *cap);

/* Capability-checked ledger calls: the same semantics as zxv_ledger_post /
 * zxv_ledger_balance, with the context taken from the cap. SDKs use these. */
ZXV_API int32_t zxv_ledger_post_cap(const zxv_cap *cap, uint32_t from_account, uint32_t to_account,
                                    int64_t amount_minor, const char *ccy, const char *reference,
                                    uint64_t *out_entry_id);
ZXV_API int32_t zxv_ledger_balance_cap(const zxv_cap *cap, uint32_t account,
                                       int64_t *out_debit_minor, int64_t *out_credit_minor,
                                       int64_t *out_equity_minor);
/* zxv_ledger_check through a cap. On ZXV_E_INVARIANT the context is
 * poisoned (see zxv_ctx_is_poisoned). */
ZXV_API int32_t zxv_ledger_check_cap(const zxv_cap *cap);

/* ===== Attestation registry (kernel/src/lpres) ===== */
typedef struct zxv_lpres zxv_lpres;

ZXV_API int32_t zxv_lpres_create(zxv_ctx *ctx, zxv_lpres **out_reg);
ZXV_API void zxv_lpres_destroy(zxv_lpres *reg);
/* name, attester <= 31 bytes; digest <= 32 bytes (may be NULL/0). subject is
 * the kernel lpres_subject_type_t (0 SOURCE .. 14 FIRMWARE). Needs ATTEST. */
ZXV_API int32_t zxv_lpres_attest(const zxv_cap *cap, zxv_lpres *reg, const char *name,
                                 int32_t subject, int32_t lp_state, const char *attester,
                                 const uint8_t *digest, uint32_t digest_len, uint32_t *out_index);
/* Add one piece of evidence; the state follows zxv_lp_from_evidence. */
ZXV_API int32_t zxv_lpres_evidence(const zxv_cap *cap, zxv_lpres *reg, uint32_t index,
                                   int32_t positive);
ZXV_API int32_t zxv_lpres_find(zxv_lpres *reg, const char *name, uint32_t *out_index);
ZXV_API int32_t zxv_lpres_state(zxv_lpres *reg, uint32_t index, int32_t *out_lp_state,
                                uint32_t *out_positive, uint32_t *out_negative);
/* 1 if every attestation is certain (TRUE or FALSE): no BOTH, no NEITHER.
 * Actuator authority requires a clear gate. */
ZXV_API int32_t zxv_lpres_gate(zxv_lpres *reg, int32_t *out_clear);

/* ===== Rational resource budgets (kernel/src/rmag, K2) ===== */
typedef struct zxv_rmag zxv_rmag;

/* rmag_resource_t, fixed. */
#define ZXV_RES_COMPUTE_CYCLES 0
#define ZXV_RES_MEMORY         1
#define ZXV_RES_BANDWIDTH      2
#define ZXV_RES_ENERGY         3
#define ZXV_RES_STORAGE        4
#define ZXV_RES_TOKENS         12 /* RMAG_RES_EVENT_COUNT: one model token = one event */

/* Listener events, delivered synchronously on the thread whose call caused
 * them, after the budget's lock is released (so the listener may call
 * back into the library). */
#define ZXV_RMAG_EV_DENIED      1 /* a consume was refused (ZXV_E_BUDGET)     */
#define ZXV_RMAG_EV_INVALID     2 /* a consume/release was malformed         */
#define ZXV_RMAG_EV_REPLENISHED 3 /* a release or new limit freed budget     */
typedef void (*zxv_rmag_listener)(void *user, uint32_t budget, int32_t event);

ZXV_API int32_t zxv_rmag_create(zxv_ctx *ctx, zxv_rmag **out_rmag);
ZXV_API void zxv_rmag_destroy(zxv_rmag *rmag);
/* A budget with an exact rational limit (>= 0). enforce = 0 only counts. */
ZXV_API int32_t zxv_rmag_budget(const zxv_cap *cap, zxv_rmag *rmag, const char *name,
                                int32_t resource, int64_t limit_num, int64_t limit_den,
                                int32_t enforce, uint32_t *out_budget);
/* Consume an amount (> 0). ZXV_E_BUDGET if it would pass an enforced limit
 * (nothing is consumed); ZXV_E_RMAG_INVALID for a negative or overflowing
 * amount. */
ZXV_API int32_t zxv_rmag_consume(const zxv_cap *cap, zxv_rmag *rmag, uint32_t budget,
                                 int64_t num, int64_t den);
/* Give back (clamped at zero consumed). Fires REPLENISHED. */
ZXV_API int32_t zxv_rmag_release(const zxv_cap *cap, zxv_rmag *rmag, uint32_t budget,
                                 int64_t num, int64_t den);
/* Replace the limit. Fires REPLENISHED when it grows. */
ZXV_API int32_t zxv_rmag_set_limit(const zxv_cap *cap, zxv_rmag *rmag, uint32_t budget,
                                   int64_t num, int64_t den);
/* Remaining = limit - consumed (exact), and consumed. Any out may be NULL.
 * *out_epoch counts REPLENISHED events, so an async waiter can tell that
 * something changed since it last looked. */
ZXV_API int32_t zxv_rmag_status(zxv_rmag *rmag, uint32_t budget, int64_t *out_rem_num,
                                int64_t *out_rem_den, int64_t *out_used_num,
                                int64_t *out_used_den, uint64_t *out_epoch);
/* One listener per zxv_rmag (NULL removes it). */
ZXV_API int32_t zxv_rmag_listen(zxv_rmag *rmag, zxv_rmag_listener fn, void *user);

/* ===== Phase coordinator ticks (kernel/src/phase_coord, K6) =====
 * ZXV time is a logical tick count, not wall-clock time. One harmonic
 * fundamental is ZXV_PHASE_CYCLE ticks (27,720 = lcm(1..11) * 1: every
 * harmonic 1..11 lands on a whole tick). A host event loop advances the
 * clock (manually, or from a timer at a chosen tick rate) and awaits
 * deferred S0 events, which clear when their tick is reached. */
typedef struct zxv_phase zxv_phase;

#define ZXV_PHASE_CYCLE 27720u

#define ZXV_ADMIT    0 /* PC_DECISION_ADMIT: token issued      */
#define ZXV_VETO     1 /* PC_DECISION_VETO: rejected           */
#define ZXV_DEFER    2 /* PC_DECISION_DEFER: parked in S0      */
#define ZXV_RETRY    3 /* PC_DECISION_RETRY                    */

#define ZXV_STEP_REQUIRES_HEALTH 0x1u
#define ZXV_STEP_HARDWARE        0x2u
#define ZXV_STEP_S0_PENDING      0x4u

#define ZXV_EV_PENDING   0 /* S0: waiting for its tick */
#define ZXV_EV_CLEARED   1
#define ZXV_EV_CANCELLED 2

#define ZXV_HEALTH_UNKNOWN   0
#define ZXV_HEALTH_HEALTHY   1
#define ZXV_HEALTH_DEGRADED  2
#define ZXV_HEALTH_UNHEALTHY 3

ZXV_API int32_t zxv_phase_create(zxv_ctx *ctx, zxv_phase **out_phase);
ZXV_API void zxv_phase_destroy(zxv_phase *phase);
ZXV_API int32_t zxv_phase_now(zxv_phase *phase, uint64_t *out_tick);
/* Advance by `delta` ticks; deferred events due at or before the new tick
 * become CLEARED. *out_cleared (may be NULL) = how many cleared now. */
ZXV_API int32_t zxv_phase_advance(const zxv_cap *cap, zxv_phase *phase, uint64_t delta,
                                  uint64_t *out_now, uint32_t *out_cleared);
/* Ask the K6 coordinator to admit a step of phase `phase_id` (1..13).
 * *out_token is non-zero only on ZXV_ADMIT. */
ZXV_API int32_t zxv_phase_admit(const zxv_cap *cap, zxv_phase *phase, uint32_t phase_id,
                                uint32_t step_id, uint32_t flags, int32_t *out_decision,
                                uint64_t *out_token);
ZXV_API int32_t zxv_phase_set_health(const zxv_cap *cap, zxv_phase *phase, uint32_t phase_id,
                                     int32_t health);
/* Park an S0 event until absolute tick `due_tick` (> now). */
ZXV_API int32_t zxv_phase_defer(const zxv_cap *cap, zxv_phase *phase, uint64_t due_tick,
                                uint64_t *out_event);
ZXV_API int32_t zxv_phase_event(zxv_phase *phase, uint64_t event, int32_t *out_state,
                                uint64_t *out_due_tick);
ZXV_API int32_t zxv_phase_cancel(const zxv_cap *cap, zxv_phase *phase, uint64_t event);
/* Earliest due tick of a PENDING event; ZXV_E_NOT_FOUND if none. */
ZXV_API int32_t zxv_phase_next_due(zxv_phase *phase, uint64_t *out_tick);

/* ===== UBH-168 harmonic wire (kernel/src/harmonic/zt_harmonic_wire.h) =====
 * 21 octets: tag, then five 32-bit words in fixed L-B-L-B-L byte order:
 * W0 little-endian (S+), W1 big-endian (S-), W2 LE (S+), W3 BE (S-),
 * W4 LE (S+). The octets are identical on every host. */
#define ZXV_WIRE_FRAME_BYTES 21u

#define ZXV_TRUTH_UNKNOWN 0
#define ZXV_TRUTH_TRUE    1
#define ZXV_TRUTH_FALSE   2
#define ZXV_TRUTH_NEUTRAL 3
#define ZXV_TRUTH_GLUT    4 /* contradiction held */
#define ZXV_TRUTH_PARADOX 5 /* persistent GLUT    */

/* tag = 100*sync + 10*shell + trunk; trunk, shell 0..9, sync 0..1. */
ZXV_API int32_t zxv_wire_tag(uint32_t trunk, uint32_t shell, uint32_t sync, uint8_t *out_tag);
ZXV_API int32_t zxv_wire_pack(uint8_t tag, const uint32_t s_plus[3], const uint32_t s_minus[2],
                              uint8_t out_frame[21]);
ZXV_API int32_t zxv_wire_unpack(const uint8_t frame[21], uint8_t *out_tag, uint32_t out_s_plus[3],
                                uint32_t out_s_minus[2]);
/* The W5 correlation resolver (words read as signed Q16.16). */
ZXV_API int32_t zxv_wire_resolve(const uint8_t frame[21], int32_t *out_truth);
/* The idle pilot frame of a trunk (never all zeros). */
ZXV_API int32_t zxv_wire_idle(uint32_t trunk, uint8_t out_frame[21]);

/* ===== Swarm token economy (kernel/src/swarm: budget + emotion) =====
 * Each cycle T tokens are split across the swarm's Fibonacci levels (the
 * real axis) and, by the mood's charge, an imaginary (emotional) share is
 * split by each model's own feeling. Feelings are emoji code points from
 * the kernel palette with intensity 0..5 (charge 0,1,2,3,5,8). All exact
 * integers. */
typedef struct zxv_swarm zxv_swarm;

ZXV_API int32_t zxv_swarm_create(zxv_ctx *ctx, uint32_t num_levels, uint64_t tokens_per_cycle,
                                 zxv_swarm **out_swarm);
ZXV_API void zxv_swarm_destroy(zxv_swarm *swarm);
/* Level d (0-based) holds at most F(d+2) models. ZXV_E_CAPACITY when full. */
ZXV_API int32_t zxv_swarm_register(const zxv_cap *cap, zxv_swarm *swarm, uint32_t model_id,
                                   uint32_t level);
ZXV_API int32_t zxv_swarm_set_active(const zxv_cap *cap, zxv_swarm *swarm, uint32_t model_id,
                                     int32_t active);
ZXV_API int32_t zxv_swarm_set_rate(const zxv_cap *cap, zxv_swarm *swarm,
                                   uint64_t tokens_per_cycle);
/* codepoint: an emoji of the palette (e.g. 0x1F914); ZXV_E_NOT_FOUND else. */
ZXV_API int32_t zxv_swarm_set_mood(const zxv_cap *cap, zxv_swarm *swarm, uint32_t codepoint,
                                   uint32_t intensity);
ZXV_API int32_t zxv_swarm_set_feeling(const zxv_cap *cap, zxv_swarm *swarm, uint32_t model_id,
                                      uint32_t codepoint, uint32_t intensity);
ZXV_API int32_t zxv_swarm_begin_cycle(const zxv_cap *cap, zxv_swarm *swarm, uint64_t *out_cycle);
/* Grant up to `requested` tokens. ZXV_OK with *out_granted = requested,
 * or ZXV_E_BUDGET with *out_granted = what was left and granted (this
 * out parameter is written on ZXV_E_BUDGET too). */
ZXV_API int32_t zxv_swarm_consume(const zxv_cap *cap, zxv_swarm *swarm, uint32_t model_id,
                                  uint64_t requested, uint64_t *out_granted);
/* This cycle: allotted (real + imaginary), its imaginary part, used. */
ZXV_API int32_t zxv_swarm_allotment(zxv_swarm *swarm, uint32_t model_id, uint64_t *out_allotted,
                                    uint64_t *out_imaginary, uint64_t *out_used);
/* Close the cycle; *out_expired = unused tokens (they never carry over). */
ZXV_API int32_t zxv_swarm_end_cycle(const zxv_cap *cap, zxv_swarm *swarm, uint64_t *out_expired);
/* Pure: capacity of level d, and an emotion's behaviour profile. */
ZXV_API int32_t zxv_swarm_level_capacity(uint32_t level, uint32_t *out_capacity);
ZXV_API int32_t zxv_swarm_emotion_profile(uint32_t codepoint, int32_t *out_valence,
                                          uint32_t *out_temperature_milli,
                                          uint32_t *out_explore_permille,
                                          uint32_t *out_verify_passes, uint32_t *out_priority);

/* ===== Hashes and secure memory ===== */
ZXV_API int32_t zxv_sha3_256(const uint8_t *data, size_t len, uint8_t out[32]);
ZXV_API int32_t zxv_shake256(const uint8_t *data, size_t len, uint8_t *out, size_t out_len);
/* Ethereum's Keccak-256 (pre-FIPS padding 0x01), for the EVM adapter. */
ZXV_API int32_t zxv_keccak256(const uint8_t *data, size_t len, uint8_t out[32]);
/* Fill with bytes from the OS CSPRNG (getrandom / arc4random). */
ZXV_API int32_t zxv_random(uint8_t *buf, size_t len);
/* Zero memory in a way the compiler may not remove. SDKs call it on every
 * buffer that held a secret (their off-heap key storage). */
ZXV_API int32_t zxv_secure_zero(void *buf, size_t len);

/* ===== Post-quantum and classical signatures / KEM (kernel/src/pqsec,
 * kernel/third_party/ed25519) =====
 * Secret keys go in caller buffers; every internal copy is wiped before
 * return. Calls are serialized internally (the vendored ML-DSA keygen is
 * not re-entrant). NOT FIPS 140-3 validated; see sdk/docs/SECURITY.md. */
#define ZXV_MLKEM1024_EK_BYTES 1568u
#define ZXV_MLKEM1024_DK_BYTES 3168u
#define ZXV_MLKEM1024_CT_BYTES 1568u
#define ZXV_MLKEM_SS_BYTES     32u
#define ZXV_MLDSA87_PK_BYTES   2592u
#define ZXV_MLDSA87_SK_BYTES   4896u
#define ZXV_MLDSA87_SIG_BYTES  4627u
#define ZXV_MLDSA_CTX_MAX      255u
#define ZXV_ED25519_PK_BYTES   32u
#define ZXV_ED25519_SK_BYTES   32u /* the RFC 8032 private key (seed) */
#define ZXV_ED25519_SIG_BYTES  64u

ZXV_API int32_t zxv_mlkem1024_keygen(const zxv_cap *cap, uint8_t ek[1568], uint8_t dk[3168]);
/* Deterministic from d and z (FIPS 203 ML-KEM.KeyGen_internal), for KATs. */
ZXV_API int32_t zxv_mlkem1024_keygen_derand(const zxv_cap *cap, const uint8_t d[32],
                                            const uint8_t z[32], uint8_t ek[1568],
                                            uint8_t dk[3168]);
/* ZXV_E_FORMAT if ek fails the FIPS 203 modulus check. */
ZXV_API int32_t zxv_mlkem1024_encaps(const zxv_cap *cap, const uint8_t ek[1568], uint8_t ct[1568],
                                     uint8_t ss[32]);
ZXV_API int32_t zxv_mlkem1024_encaps_derand(const zxv_cap *cap, const uint8_t ek[1568],
                                            const uint8_t m[32], uint8_t ct[1568],
                                            uint8_t ss[32]);
ZXV_API int32_t zxv_mlkem1024_decaps(const zxv_cap *cap, const uint8_t dk[3168],
                                     const uint8_t ct[1568], uint8_t ss[32]);

ZXV_API int32_t zxv_mldsa87_keygen(const zxv_cap *cap, uint8_t pk[2592], uint8_t sk[4896]);
ZXV_API int32_t zxv_mldsa87_keygen_seed(const zxv_cap *cap, const uint8_t seed[32],
                                        uint8_t pk[2592], uint8_t sk[4896]);
/* Hedged signing (fresh randomness). ctx: FIPS 204 context string, may be
 * NULL/0, at most 255 bytes. */
ZXV_API int32_t zxv_mldsa87_sign(const zxv_cap *cap, const uint8_t sk[4896], const uint8_t *msg,
                                 size_t msg_len, const uint8_t *ctx, size_t ctx_len,
                                 uint8_t sig[4627]);
/* Deterministic signing (same input, same signature), for KATs. */
ZXV_API int32_t zxv_mldsa87_sign_deterministic(const zxv_cap *cap, const uint8_t sk[4896],
                                               const uint8_t *msg, size_t msg_len,
                                               const uint8_t *ctx, size_t ctx_len,
                                               uint8_t sig[4627]);
/* ZXV_OK if valid, ZXV_E_VERIFY if not. */
ZXV_API int32_t zxv_mldsa87_verify(const zxv_cap *cap, const uint8_t pk[2592], const uint8_t *msg,
                                   size_t msg_len, const uint8_t *ctx, size_t ctx_len,
                                   const uint8_t sig[4627]);

ZXV_API int32_t zxv_ed25519_keygen(const zxv_cap *cap, uint8_t pk[32], uint8_t sk[32]);
ZXV_API int32_t zxv_ed25519_public(const zxv_cap *cap, const uint8_t sk[32], uint8_t pk[32]);
ZXV_API int32_t zxv_ed25519_sign(const zxv_cap *cap, const uint8_t sk[32], const uint8_t *msg,
                                 size_t msg_len, uint8_t sig[64]);
ZXV_API int32_t zxv_ed25519_verify(const zxv_cap *cap, const uint8_t pk[32], const uint8_t *msg,
                                   size_t msg_len, const uint8_t sig[64]);

/* ===== Integer inference engine (kernel/src/tensor) =====
 * GGUF models (qwen2, qwen3, llama; F32/F16/BF16/Q8_0/Q4_0/Q5_x/Q4_K/Q6_K)
 * run in integer arithmetic only, bit-identical on every machine. A model
 * is read-only after loading and may be shared by sessions on any threads;
 * a session (KV cache) is used by one thread at a time. */
typedef struct zxv_model zxv_model;
typedef struct zxv_session zxv_session;
typedef struct zxv_lora zxv_lora;

#define ZXV_KV_Q16 0 /* exact KV cache, 4 bytes per value       */
#define ZXV_KV_Q8  1 /* 8-bit KV cache, ~1.16 bytes per value   */

#define ZXV_MODEL_ARCH        1 /* 1 qwen2, 2 qwen3, 3 llama */
#define ZXV_MODEL_N_VOCAB     2
#define ZXV_MODEL_N_CTX_TRAIN 3
#define ZXV_MODEL_N_LAYER     4
#define ZXV_MODEL_N_EMBD      5
#define ZXV_MODEL_N_HEAD      6
#define ZXV_MODEL_N_HEAD_KV   7
#define ZXV_MODEL_EOS         8

#define ZXV_STOP_EOS      1 /* end-of-sequence token          */
#define ZXV_STOP_MAX      2 /* max_tokens reached             */
#define ZXV_STOP_STR      3 /* reserved                       */
#define ZXV_STOP_CALLBACK 4 /* the token callback returned 0  */
#define ZXV_STOP_CTX      5 /* the context window is full     */
#define ZXV_STOP_BUDGET   6 /* the bound rmag budget ran out  */

/* Called once per generated token with its bytes (UTF-8, possibly a
 * partial character). Return non-zero to continue, 0 to stop. Runs on the
 * thread that called zxv_generate. */
typedef int32_t (*zxv_token_fn)(void *user, int32_t token, const uint8_t *piece, size_t len);

/* Open a GGUF file. tokenizer_path NULL = the model file's own tokenizer. */
ZXV_API int32_t zxv_model_open(const zxv_cap *cap, const char *model_path,
                               const char *tokenizer_path, zxv_model **out_model);
/* The same from memory; the bytes are copied, so the caller may free them. */
ZXV_API int32_t zxv_model_open_memory(const zxv_cap *cap, const uint8_t *model, size_t model_len,
                                      const uint8_t *tokenizer, size_t tokenizer_len,
                                      zxv_model **out_model);
ZXV_API void zxv_model_destroy(zxv_model *model);
ZXV_API int32_t zxv_model_info(const zxv_model *model, int32_t key, int64_t *out_value);

/* Token ids of UTF-8 text (R4 two-call pattern on *out_count). */
ZXV_API int32_t zxv_tokenize(const zxv_model *model, const uint8_t *text, size_t len,
                             int32_t parse_special, int32_t *ids, size_t cap, size_t *out_count);
ZXV_API int32_t zxv_detokenize(const zxv_model *model, const int32_t *ids, size_t count,
                               char *buf, size_t cap, size_t *out_len);

/* n_ctx 0 = min(n_ctx_train, 4096). */
ZXV_API int32_t zxv_session_create(const zxv_model *model, uint32_t n_ctx, int32_t kv_type,
                                   zxv_session **out_session);
ZXV_API void zxv_session_destroy(zxv_session *session);
/* Forget the conversation (empty KV cache). */
ZXV_API int32_t zxv_session_reset(zxv_session *session);
ZXV_API int32_t zxv_session_position(zxv_session *session, uint32_t *out_pos);
/* Charge this budget one unit per prompt and generated token (rmag NULL
 * unbinds). Prompt tokens are charged up front: if they do not fit,
 * zxv_generate returns ZXV_E_BUDGET and runs nothing. */
ZXV_API int32_t zxv_session_bind_budget(zxv_session *session, zxv_rmag *rmag, uint32_t budget);

/* Generate. Sampling parameters are Q16.16 (temperature_q16 0 = greedy;
 * top_p_q16 0 = off; repeat_penalty_q16 0 = off). The same seed and inputs
 * give the same tokens on every machine. Appends to the session, so a chat
 * continues turn after turn. Any out pointer may be NULL. */
ZXV_API int32_t zxv_generate(const zxv_cap *cap, zxv_session *session, const uint8_t *prompt,
                             size_t prompt_len, uint32_t max_tokens, int32_t temperature_q16,
                             uint32_t top_k, int32_t top_p_q16, int32_t repeat_penalty_q16,
                             uint32_t repeat_last_n, uint64_t seed, zxv_token_fn on_token,
                             void *user, uint32_t *out_n_prompt, uint32_t *out_n_generated,
                             int32_t *out_stop);

/* LoRA adapters: declared for the ABI, NOT implemented by the engine yet
 * (ZXV_FEAT_LORA is never set; these return ZXV_E_NOTIMPL). */
ZXV_API int32_t zxv_lora_load(const zxv_cap *cap, zxv_model *model, const char *path,
                              int32_t scale_q16, zxv_lora **out_lora);
ZXV_API int32_t zxv_lora_apply(const zxv_cap *cap, zxv_session *session, zxv_lora *lora);
ZXV_API void zxv_lora_destroy(zxv_lora *lora);

/* ===== EVM state channels =====
 * The ledger's exact integer balances, encoded for a Solidity state-channel
 * contract (sdk/solidity/ZxvStateChannel.sol) as
 *   abi.encode(bytes32 channelId, uint256 nonce, address[] parties,
 *              int256[] balances)
 * and digest = keccak256(encoding). The parties sign the digest with their
 * own Ethereum keys (EIP-191) in the host wallet; ZXV has no secp256k1. */
ZXV_API int32_t zxv_evm_encode_state(const zxv_cap *cap, const uint8_t channel_id[32],
                                     uint64_t nonce, const uint8_t *addresses,
                                     const int64_t *balances, uint32_t count, uint8_t *buf,
                                     size_t cap_bytes, size_t *out_len, uint8_t digest[32]);
/* The same, reading each party's EQUITY (888) balance from ledger accounts
 * (all in `ccy`). Needs EVM and LEDGER_READ. */
ZXV_API int32_t zxv_evm_ledger_state(const zxv_cap *cap, const uint8_t channel_id[32],
                                     uint64_t nonce, const char *ccy, const uint32_t *accounts,
                                     const uint8_t *addresses, uint32_t count, uint8_t *buf,
                                     size_t cap_bytes, size_t *out_len, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif /* ZXV_API_H */
