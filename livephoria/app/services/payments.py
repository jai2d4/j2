"""Money movement.

Nothing here talks to a card network. Balances move inside this database and a
`LedgerEntry` row records every movement, which is what a real integration would
reconcile against. `TopUpProvider` is the seam: swap `MockTopUpProvider` for a
Stripe/Adyen-backed implementation and the rest of the app is unchanged.

Rules that hold everywhere:
  * amounts are integer cents; no floats touch a balance
  * the platform fee is taken from the gross, so net = gross - fee
  * a charge that would overdraw a wallet raises `InsufficientFunds` and writes nothing
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

from sqlalchemy import select

from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import get_settings
from app.models.orm import CreatorProfile, LedgerEntry, Payout, PayoutAccount, User


class PaymentError(Exception):
    """Base for anything that stops a charge."""


class PayoutNotPermitted(PaymentError):
    """Raised when a withdrawal is blocked by identity checks, not by balance."""


class InsufficientFunds(PaymentError):
    def __init__(self, needed_cents: int, available_cents: int) -> None:
        self.needed_cents = needed_cents
        self.available_cents = available_cents
        short = needed_cents - available_cents
        super().__init__(f"Wallet is short {short} cents for a {needed_cents} cent charge")


@dataclass(frozen=True)
class Split:
    gross_cents: int
    fee_cents: int
    net_cents: int


def split_revenue(gross_cents: int, fee_bps: int | None = None) -> Split:
    """Platform fee rounds down, so the creator is never short a cent from rounding."""
    if gross_cents < 0:
        raise ValueError("gross_cents cannot be negative")
    bps = get_settings().platform_fee_bps if fee_bps is None else fee_bps
    fee = (gross_cents * bps) // 10_000
    return Split(gross_cents=gross_cents, fee_cents=fee, net_cents=gross_cents - fee)


class TopUpProvider(Protocol):
    async def charge(self, user: User, amount_cents: int) -> str:
        """Take money from a real funding source and return a provider reference."""


class MockTopUpProvider:
    """Simulated funding. Always succeeds, charges nobody, moves no real money."""

    name = "mock"

    async def charge(self, user: User, amount_cents: int) -> str:
        return f"mock_topup_{user.id}_{amount_cents}"


def get_topup_provider() -> TopUpProvider:
    configured = get_settings().payments_provider
    if configured != "mock":
        raise RuntimeError(
            f"PAYMENTS_PROVIDER={configured!r} has no implementation wired up; "
            "only 'mock' ships with this build"
        )
    return MockTopUpProvider()


async def top_up(session: AsyncSession, user: User, amount_cents: int) -> LedgerEntry:
    if amount_cents <= 0:
        raise PaymentError("Top-up must be positive")
    provider = get_topup_provider()
    reference = await provider.charge(user, amount_cents)

    user.wallet_balance_cents += amount_cents
    entry = LedgerEntry(
        kind="topup",
        payer_id=user.id,
        gross_cents=amount_cents,
        fee_cents=0,
        net_cents=amount_cents,
        reference_type="provider",
        note=reference,
    )
    session.add(entry)
    return entry


async def charge(
    session: AsyncSession,
    *,
    payer: User,
    creator: CreatorProfile,
    creator_user: User,
    amount_cents: int,
    kind: str,
    reference_type: str | None = None,
    reference_id: int | None = None,
    note: str = "",
) -> LedgerEntry:
    """Move `amount_cents` from a fan's wallet to a creator's earnings, minus the fee.

    Caller is responsible for the commit, so a purchase and its ledger entry land
    in the same transaction.
    """
    if amount_cents < 0:
        raise PaymentError("Charge cannot be negative")
    if amount_cents > 0 and payer.wallet_balance_cents < amount_cents:
        raise InsufficientFunds(amount_cents, payer.wallet_balance_cents)

    split = split_revenue(amount_cents)
    payer.wallet_balance_cents -= split.gross_cents
    creator_user.earnings_balance_cents += split.net_cents

    entry = LedgerEntry(
        kind=kind,
        payer_id=payer.id,
        creator_id=creator.id,
        reference_type=reference_type,
        reference_id=reference_id,
        gross_cents=split.gross_cents,
        fee_cents=split.fee_cents,
        net_cents=split.net_cents,
        note=note,
    )
    session.add(entry)
    return entry


async def payout(
    session: AsyncSession, creator_user: User, amount_cents: int
) -> tuple[Payout, LedgerEntry]:
    """Request a withdrawal.

    The money leaves the creator's earnings balance and the request is recorded
    as `pending`. Nothing here settles it: no bank transfer happens in this build,
    and a real provider would move a payout to `paid` or `failed` afterwards.

    Withdrawals are gated on an approved payout account. This is the point where
    a platform's identity and sanctions obligations bite, so the check is here
    rather than left to the caller.
    """
    if amount_cents <= 0:
        raise PaymentError("Payout must be positive")

    account = (
        await session.execute(select(PayoutAccount).where(PayoutAccount.user_id == creator_user.id))
    ).scalar_one_or_none()
    if account is None:
        raise PayoutNotPermitted("Add your payout details before withdrawing")
    if account.status != "approved":
        raise PayoutNotPermitted(
            {
                "pending": "Your payout details are still being reviewed",
                "rejected": "Your payout details were rejected — update them and resubmit",
            }.get(account.status, "Your payout account is not approved")
        )

    if creator_user.earnings_balance_cents < amount_cents:
        raise InsufficientFunds(amount_cents, creator_user.earnings_balance_cents)

    creator_user.earnings_balance_cents -= amount_cents
    record = Payout(user_id=creator_user.id, amount_cents=amount_cents, status="pending")
    session.add(record)

    entry = LedgerEntry(
        kind="payout",
        payer_id=None,
        creator_id=creator_user.creator.id if creator_user.creator else None,
        gross_cents=amount_cents,
        fee_cents=0,
        net_cents=amount_cents,
        note="payout requested",
    )
    session.add(entry)
    return record, entry


async def reverse_payout(session: AsyncSession, creator_user: User, record: Payout) -> LedgerEntry:
    """Put a failed payout back. The failed row stays, so the attempt is on record."""
    creator_user.earnings_balance_cents += record.amount_cents
    entry = LedgerEntry(
        kind="payout_reversal",
        creator_id=creator_user.creator.id if creator_user.creator else None,
        gross_cents=record.amount_cents,
        fee_cents=0,
        net_cents=record.amount_cents,
        note=f"payout {record.id} failed",
    )
    session.add(entry)
    return entry
