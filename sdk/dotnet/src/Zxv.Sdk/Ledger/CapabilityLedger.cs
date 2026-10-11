// Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC
// SPDX-License-Identifier: Apache-2.0

using System;
using Zxv.Interop;

namespace Zxv.Ledger;

/// <summary>
/// The ledger seen through a <see cref="Capability"/>: the same posting and
/// balance semantics as <see cref="LedgerService"/>, but every call presents
/// the capability and the native library checks it (posting needs
/// <see cref="CapabilityRights.LedgerPost"/>, reading
/// <see cref="CapabilityRights.LedgerRead"/>). Obtain it with
/// <see cref="LedgerService.WithCapability(Capability)"/> and hand it to code
/// that should hold no more than that capability. Thread-safe.
/// </summary>
public sealed class CapabilityLedger
{
    private readonly LedgerService _ledger;
    private readonly Capability _cap;

    internal CapabilityLedger(LedgerService ledger, Capability capability)
    {
        _ledger = ledger;
        _cap = capability;
    }

    /// <summary>Gets the capability presented on every call.</summary>
    public Capability Capability => _cap;

    /// <summary>Posts a balanced transfer (see <see cref="LedgerService.Post(Transfer)"/>).</summary>
    /// <param name="transfer">The transfer.</param>
    /// <returns>The posting receipt.</returns>
    /// <exception cref="CapabilityDeniedException">The capability cannot post.</exception>
    /// <exception cref="InsufficientFundsException">A holder would go below zero.</exception>
    public PostingReceipt Post(Transfer transfer)
    {
        ArgumentNullException.ThrowIfNull(transfer);
        Guard.Text(transfer.Reference, nameof(transfer));
        Guard.Text(transfer.Amount.Currency, nameof(transfer));
        _cap.Enter("Ledger.Post");
        int rc = NativeMethods.LedgerPostCap(_cap.Handle, transfer.From.Value, transfer.To.Value, transfer.Amount.MinorUnits, transfer.Amount.Currency, transfer.Reference, out ulong entryId);
        _cap.Check(rc, "Ledger.Post");
        return new PostingReceipt(entryId, transfer);
    }

    /// <summary>Gets the three rail balances of an account.</summary>
    /// <param name="id">The account.</param>
    /// <returns>The balances.</returns>
    public RailBalances GetBalances(AccountId id)
    {
        _cap.Enter("Ledger.GetBalances");
        _cap.Check(NativeMethods.LedgerBalanceCap(_cap.Handle, id.Value, out long d, out long c, out long e), "Ledger.GetBalances");
        Account account = _ledger.GetAccount(id);
        byte scale = _ledger.ScaleOf(account.Currency);
        return new RailBalances(new Money(d, account.Currency, scale), new Money(c, account.Currency, scale), new Money(e, account.Currency, scale));
    }

    /// <summary>
    /// Verifies the ledger invariants. A failure halts the context: it raises
    /// <see cref="ZxvFatalException"/> and every later call fails the same way.
    /// </summary>
    public void Verify()
    {
        _cap.Enter("Ledger.Verify");
        _cap.Check(NativeMethods.LedgerCheckCap(_cap.Handle), "Ledger.Verify");
    }
}
