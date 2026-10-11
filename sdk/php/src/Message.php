<?php

// Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC
// SPDX-License-Identifier: Apache-2.0

declare(strict_types=1);

namespace Zedec\Zxv;

/**
 * An ISO 20022 message under construction (pacs.008, camt.053; other kinds
 * throw NotImplementedException until the kernel builds them).
 * Serialization only: libzxv has no network transport.
 */
final class Message extends NativeHandle
{
    public const PACS008 = 1;
    public const CAMT053 = 2;
    public const PACS002 = 3;
    public const PACS004 = 4;
    public const PACS009 = 5;
    public const CAMT056 = 6;
    public const CAMT029 = 7;

    public const MSG_ID = 1;
    public const CRE_DT_TM = 2;
    public const DEBTOR_NAME = 3;
    public const DEBTOR_ACCT = 4;
    public const CREDITOR_NAME = 5;
    public const CREDITOR_ACCT = 6;
    public const END_TO_END_ID = 7;
    public const CCY = 8;
    public const ACCT_ID = 9;

    public const AMT_SETTLEMENT = 1;
    public const AMT_OPENING = 2;
    public const AMT_CLOSING = 3;

    public function __construct(Context $context, int $kind)
    {
        $out = Native::new("zxv_msg*");
        Native::check(Native::ffi()->zxv_msg_create($context->raw(), $kind, \FFI::addr($out)), $context);
        parent::__construct($context, $out, "zxv_msg_destroy");
    }

    /** Set a text field (plain text; the serializer escapes XML). */
    public function set(int $field, string $value): self
    {
        Native::check(Native::ffi()->zxv_msg_set_text($this->ptr(), $field, Native::utf8($value, "value")), $this->context);
        return $this;
    }

    /** Set an amount in exact minor units with $fracDigits decimals. */
    public function amount(int $field, int|float $units, int $fracDigits): self
    {
        Native::check(Native::ffi()->zxv_msg_set_amount($this->ptr(), $field, Native::int($units, "units"), Native::int($fracDigits, "fracDigits", 0, 255)), $this->context);
        return $this;
    }

    /** Serialize to XML. */
    public function render(): string
    {
        [$rc, $xml] = Native::text(fn ($b, $c, $l) => Native::ffi()->zxv_msg_render($this->ptr(), $b, $c, $l));
        Native::check($rc, $this->context);
        return (string) $xml;
    }
}
