"""What a person can do about content or a decision: report, verify age, appeal.

The other half — the queue these feed, and the decisions taken on them — lives in
routers/control.py, behind the Axiome shared secret.
"""
from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import select

from app.core.deps import CreatorDep, OptionalUserDep, SessionDep, UserDep
from app.models.orm import Appeal, ModerationAction, PayoutAccount, Payout, Report, User
from app.models.schemas import (
    ActionOut,
    AgeVerificationOut,
    AgeVerificationRequest,
    AppealCreate,
    AppealOut,
    PayoutAccountCreate,
    PayoutAccountOut,
    PayoutOut,
    ReportCreate,
    ReportOut,
)
from app.services import axiome, moderation, verification

router = APIRouter(prefix="/api/v1", tags=["safety"])


def report_out(report: Report, reporter: User | None = None) -> ReportOut:
    return ReportOut(
        id=report.id,
        target_type=report.target_type,
        target_id=report.target_id,
        reason=report.reason,
        reason_label=moderation.REASONS.get(report.reason, report.reason),
        detail=report.detail,
        priority=report.priority,
        status=report.status,
        created_at=report.created_at,
        resolved_at=report.resolved_at,
        resolution=report.resolution,
        reporter_display_name=reporter.display_name if reporter else None,
    )


@router.get("/report-reasons")
async def report_reasons() -> list[dict]:
    """The closed list of reasons, so the UI and the queue agree on the words."""
    return [
        {"reason": key, "label": label, "urgent": key in moderation.URGENT_REASONS}
        for key, label in moderation.REASONS.items()
    ]


@router.post("/reports", response_model=ReportOut, status_code=status.HTTP_201_CREATED)
async def submit_report(
    payload: ReportCreate, session: SessionDep, reporter: OptionalUserDep
) -> ReportOut:
    """File a report.

    Signed-out reports are accepted: requiring an account to flag serious content
    would suppress exactly the reports that matter most. Nothing is auto-actioned
    — this only puts the item in the review queue.
    """
    if payload.target_type not in moderation.TARGET_TYPES:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "Unknown target type")
    if payload.reason not in moderation.REASONS:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "Unknown reason")
    if not await moderation.target_exists(session, payload.target_type, payload.target_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "That content no longer exists")

    report = Report(
        reporter_id=reporter.id if reporter else None,
        target_type=payload.target_type,
        target_id=payload.target_id,
        reason=payload.reason,
        detail=payload.detail,
        priority=moderation.priority_for(payload.reason),
    )
    session.add(report)
    await session.commit()
    await session.refresh(report)

    if report.priority == "urgent":
        # Push rather than wait for the next heartbeat — for these reasons the
        # delay is the harm.
        await axiome.client().emit(
            "moderation.urgent_report",
            {
                "report_id": report.id,
                "reason": report.reason,
                "target_type": report.target_type,
                "target_id": report.target_id,
            },
        )
    return report_out(report, reporter)


@router.get("/me/reports", response_model=list[ReportOut])
async def my_reports(user: UserDep, session: SessionDep) -> list[ReportOut]:
    result = await session.execute(
        select(Report).where(Report.reporter_id == user.id).order_by(Report.created_at.desc())
    )
    return [report_out(r, user) for r in result.scalars().all()]


@router.get("/me/actions", response_model=list[ActionOut])
async def actions_against_me(user: UserDep, session: SessionDep) -> list[ActionOut]:
    """What has been done to your account or content, and whether you can appeal it."""
    result = await session.execute(
        select(ModerationAction)
        .where(ModerationAction.subject_user_id == user.id)
        .order_by(ModerationAction.created_at.desc())
    )
    actions = list(result.scalars().all())
    appeals = await session.execute(select(Appeal).where(Appeal.user_id == user.id))
    by_action = {a.action_id: a.status for a in appeals.scalars().all()}
    return [
        ActionOut(
            id=a.id,
            action=a.action,
            target_type=a.target_type,
            target_id=a.target_id,
            reason=moderation.REASONS.get(a.reason, a.reason),
            note=a.note,
            created_at=a.created_at,
            appealable=a.appealable,
            appeal_status=by_action.get(a.id),
        )
        for a in actions
    ]


@router.post("/appeals", response_model=AppealOut, status_code=status.HTTP_201_CREATED)
async def submit_appeal(payload: AppealCreate, user: UserDep, session: SessionDep) -> AppealOut:
    action = await session.get(ModerationAction, payload.action_id)
    if action is None or action.subject_user_id != user.id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such decision on your account")
    if not action.appealable:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "That decision can't be appealed")

    existing = await session.execute(
        select(Appeal).where(Appeal.action_id == action.id, Appeal.user_id == user.id)
    )
    if existing.scalar_one_or_none():
        raise HTTPException(status.HTTP_409_CONFLICT, "You've already appealed this")

    appeal = Appeal(action_id=action.id, user_id=user.id, body=payload.body)
    session.add(appeal)
    await session.commit()
    await session.refresh(appeal)
    await axiome.client().emit(
        "moderation.appeal_filed", {"appeal_id": appeal.id, "action": action.action}
    )
    return AppealOut(
        id=appeal.id,
        action_id=action.id,
        action=action.action,
        target_type=action.target_type,
        target_id=action.target_id,
        body=appeal.body,
        status=appeal.status,
        created_at=appeal.created_at,
        user_display_name=user.display_name,
    )


@router.get("/me/appeals", response_model=list[AppealOut])
async def my_appeals(user: UserDep, session: SessionDep) -> list[AppealOut]:
    result = await session.execute(
        select(Appeal, ModerationAction)
        .join(ModerationAction, ModerationAction.id == Appeal.action_id)
        .where(Appeal.user_id == user.id)
        .order_by(Appeal.created_at.desc())
    )
    return [
        AppealOut(
            id=ap.id,
            action_id=ac.id,
            action=ac.action,
            target_type=ac.target_type,
            target_id=ac.target_id,
            body=ap.body,
            status=ap.status,
            created_at=ap.created_at,
            decided_at=ap.decided_at,
            decision_note=ap.decision_note,
            user_display_name=user.display_name,
        )
        for ap, ac in result.all()
    ]


# ---------------------------------------------------------------- age assurance


@router.get("/me/age-verification", response_model=AgeVerificationOut)
async def age_status(user: UserDep) -> AgeVerificationOut:
    return AgeVerificationOut(status=user.age_check_status, verified_at=user.age_verified_at)


@router.post("/me/age-verification", response_model=AgeVerificationOut)
async def submit_age_verification(
    payload: AgeVerificationRequest, user: UserDep, session: SessionDep
) -> AgeVerificationOut:
    """Submit for age assurance.

    A declared birthday under 18 is refused outright. Being over 18 by that
    declaration is not proof of anything — it only lets the submission through to
    the provider, which is what actually decides.
    """
    if user.age_check_status == "verified":
        return AgeVerificationOut(
            status="verified", verified_at=user.age_verified_at, message="Already verified"
        )
    if not verification.declared_age_is_adult(payload.date_of_birth):
        user.age_check_status = "failed"
        await session.commit()
        raise HTTPException(
            status.HTTP_403_FORBIDDEN,
            f"You must be {verification.MINIMUM_AGE} or over",
        )

    result = await verification.get_provider().submit(user.id, payload.date_of_birth)
    user.age_check_status = result.status
    user.age_check_reference = payload.document_reference or result.reference
    if result.status == "verified":
        user.age_verified_at = datetime.now(timezone.utc)
    await session.commit()
    return AgeVerificationOut(
        status=user.age_check_status, verified_at=user.age_verified_at, message=result.message
    )


# ---------------------------------------------------------------- payouts


def payout_account_out(account: PayoutAccount | None) -> PayoutAccountOut:
    if account is None:
        return PayoutAccountOut(
            status="unstarted", legal_name="", country="", can_withdraw=False
        )
    return PayoutAccountOut(
        status=account.status,
        legal_name=account.legal_name,
        country=account.country,
        note=account.note,
        reviewed_at=account.reviewed_at,
        can_withdraw=account.status == "approved",
    )


@router.get("/me/payout-account", response_model=PayoutAccountOut)
async def get_payout_account(
    creator: CreatorDep, user: UserDep, session: SessionDep
) -> PayoutAccountOut:
    account = await session.execute(select(PayoutAccount).where(PayoutAccount.user_id == user.id))
    return payout_account_out(account.scalar_one_or_none())


@router.post("/me/payout-account", response_model=PayoutAccountOut, status_code=201)
async def submit_payout_account(
    payload: PayoutAccountCreate, creator: CreatorDep, user: UserDep, session: SessionDep
) -> PayoutAccountOut:
    """Start or resubmit identity checks for withdrawals.

    Only a name and country are held here. Bank details, tax identifiers, and
    documents belong with the payment provider, not in this database.
    """
    result = await session.execute(select(PayoutAccount).where(PayoutAccount.user_id == user.id))
    account = result.scalar_one_or_none()
    if account is None:
        account = PayoutAccount(user_id=user.id, legal_name=payload.legal_name, country=payload.country)
        session.add(account)
    else:
        if account.status == "approved":
            raise HTTPException(status.HTTP_409_CONFLICT, "Your payout account is already approved")
        account.legal_name = payload.legal_name
        account.country = payload.country
        account.status = "pending"
        account.note = ""
        account.reviewed_at = None
    await session.commit()
    await session.refresh(account)
    await axiome.client().emit("kyc.submitted", {"user_id": user.id, "country": account.country})
    return payout_account_out(account)


@router.get("/me/payouts", response_model=list[PayoutOut])
async def my_payouts(creator: CreatorDep, user: UserDep, session: SessionDep) -> list[PayoutOut]:
    result = await session.execute(
        select(Payout).where(Payout.user_id == user.id).order_by(Payout.created_at.desc())
    )
    return [
        PayoutOut(
            id=p.id,
            amount_cents=p.amount_cents,
            status=p.status,
            created_at=p.created_at,
            settled_at=p.settled_at,
            note=p.note,
            provider_reference=p.provider_reference,
        )
        for p in result.scalars().all()
    ]
