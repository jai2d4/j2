"""Trust and safety: what can be reported, how urgent it is, and what a decision does.

Two principles the rest of the code depends on:

  * Nothing is auto-actioned. A report changes a queue, never content. A person
    (or Axiome acting for one) decides, and the decision is recorded as a
    ModerationAction that names a subject and can be appealed.
  * Removal is reversible. Content is marked `removed`, never deleted, so an
    upheld appeal can put it back exactly as it was.
"""
from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy.ext.asyncio import AsyncSession

from app.models.orm import Comment, CreatorProfile, ModerationAction, Post, Report, User

# The reasons a person can pick. A closed list keeps the queue sortable and
# stops the reason field turning into free text nobody can triage.
REASONS: dict[str, str] = {
    "csam": "Sexual content involving a minor",
    "nonconsensual": "Shared without the subject's consent",
    "underage": "The person in this content is under 18",
    "threat": "Threat of violence or self-harm",
    "harassment": "Harassment or hate",
    "impersonation": "Pretending to be someone else",
    "copyright": "Uses my work without permission",
    "spam": "Spam or a scam",
    "other": "Something else",
}

# These are the ones where a delay is itself the harm. They sort to the top of
# the queue and are pushed to the controller the moment they arrive.
URGENT_REASONS = {"csam", "nonconsensual", "underage", "threat"}

TARGET_TYPES = {"post", "comment", "creator", "message"}

ACTIONS = {
    "remove_content",
    "restore_content",
    "suspend_user",
    "restore_user",
    "dismiss",
}


def priority_for(reason: str) -> str:
    return "urgent" if reason in URGENT_REASONS else "normal"


async def subject_of(session: AsyncSession, target_type: str, target_id: int) -> User | None:
    """Whose account an action against this target lands on."""
    if target_type == "post":
        post = await session.get(Post, target_id)
        if post is None:
            return None
        creator = await session.get(CreatorProfile, post.creator_id)
        return await session.get(User, creator.user_id) if creator else None
    if target_type == "comment":
        comment = await session.get(Comment, target_id)
        return await session.get(User, comment.user_id) if comment else None
    if target_type == "creator":
        creator = await session.get(CreatorProfile, target_id)
        return await session.get(User, creator.user_id) if creator else None
    return None


async def target_exists(session: AsyncSession, target_type: str, target_id: int) -> bool:
    if target_type == "post":
        return await session.get(Post, target_id) is not None
    if target_type == "comment":
        return await session.get(Comment, target_id) is not None
    if target_type == "creator":
        return await session.get(CreatorProfile, target_id) is not None
    if target_type == "message":
        from app.models.orm import Message

        return await session.get(Message, target_id) is not None
    return False


async def apply_action(
    session: AsyncSession,
    *,
    action: str,
    target_type: str,
    target_id: int,
    reason: str = "",
    note: str = "",
    actor: str = "axiome",
    report: Report | None = None,
) -> ModerationAction:
    """Carry out a decision and record it. The caller commits."""
    if action not in ACTIONS:
        raise ValueError(f"Unknown action {action!r}")

    subject = await subject_of(session, target_type, target_id)

    if action in {"remove_content", "restore_content"}:
        new_status = "removed" if action == "remove_content" else "visible"
        if target_type == "post":
            post = await session.get(Post, target_id)
            if post is not None:
                post.status = new_status
        elif target_type == "comment":
            comment = await session.get(Comment, target_id)
            if comment is not None:
                comment.status = new_status
    elif action in {"suspend_user", "restore_user"} and subject is not None:
        subject.is_suspended = action == "suspend_user"

    record = ModerationAction(
        report_id=report.id if report else None,
        target_type=target_type,
        target_id=target_id,
        action=action,
        reason=reason or (report.reason if report else ""),
        note=note,
        actor=actor,
        subject_user_id=subject.id if subject else None,
        # A dismissal is not something the reported person needs to appeal.
        appealable=action not in {"dismiss", "restore_content", "restore_user"},
    )
    session.add(record)

    if report is not None:
        report.status = "dismissed" if action == "dismiss" else "actioned"
        report.resolved_at = datetime.now(timezone.utc)
        report.resolution = note[:300]

    return record


def reversal_of(action: str) -> str | None:
    """What undoes an action, for an appeal that is upheld."""
    return {"remove_content": "restore_content", "suspend_user": "restore_user"}.get(action)
