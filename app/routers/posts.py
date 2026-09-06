"""Posts: publishing, the feed, and unlocking."""
from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query, status
from sqlalchemy import delete, select

from app.core.deps import CreatorDep, OptionalUserDep, SessionDep, UserDep
from app.models.orm import (
    Comment,
    CreatorProfile,
    Follow,
    Post,
    PostLike,
    PostUnlock,
    Subscription,
    Tier,
    User,
)
from app.models.schemas import (
    CommentCreate,
    CommentOut,
    PostCreate,
    PostOut,
    PurchaseResult,
)
from app.services import catalog, payments
from app.services.access import decide_post_access, money

router = APIRouter(prefix="/api/v1", tags=["posts"])


@router.post("/posts", response_model=PostOut, status_code=status.HTTP_201_CREATED)
async def create_post(
    payload: PostCreate, creator: CreatorDep, user: UserDep, session: SessionDep
) -> PostOut:
    if payload.visibility == "ppv" and payload.price_cents <= 0:
        raise HTTPException(
            status.HTTP_422_UNPROCESSABLE_ENTITY, "A pay-per-view post needs a price"
        )
    if payload.is_adult and user.age_check_status != "verified":
        raise HTTPException(
            status.HTTP_403_FORBIDDEN, "Verify your age before publishing 18+ content"
        )
    if payload.min_tier_id is not None:
        tier = await session.get(Tier, payload.min_tier_id)
        if tier is None or tier.creator_id != creator.id:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "That tier is not yours")

    post = Post(creator_id=creator.id, **payload.model_dump())
    session.add(post)
    await session.commit()
    await session.refresh(post)
    # The author always sees their own post unlocked.
    return catalog.serialize_post(
        post,
        catalog.author_of(creator, user),
        decide_post_access(is_owner=True, visibility=post.visibility),
    )


@router.get("/feed", response_model=list[PostOut])
async def feed(
    session: SessionDep,
    viewer: OptionalUserDep,
    limit: int = Query(30, ge=1, le=100),
    offset: int = Query(0, ge=0),
) -> list[PostOut]:
    """Signed in: creators you follow or subscribe to. Signed out: what's public."""
    query = (
        select(Post)
        .where(Post.status == "visible")
        .order_by(Post.created_at.desc(), Post.id.desc())
    )

    if viewer is not None:
        followed = await session.execute(
            select(Follow.creator_id).where(Follow.fan_id == viewer.id)
        )
        subscribed = await session.execute(
            select(Subscription.creator_id).where(
                Subscription.fan_id == viewer.id, Subscription.status == "active"
            )
        )
        creator_ids = set(followed.scalars().all()) | set(subscribed.scalars().all())
        if viewer.creator is not None:
            creator_ids.add(viewer.creator.id)
        if creator_ids:
            query = query.where(Post.creator_id.in_(creator_ids))
        else:
            # Nothing followed yet — show the public firehose rather than an empty screen.
            query = query.where(Post.visibility == "public")
    else:
        query = query.where(Post.visibility == "public")

    result = await session.execute(query.limit(limit).offset(offset))
    posts = list(result.scalars().all())
    creators, creator_users = await catalog.load_creator_context(
        session, {p.creator_id for p in posts}
    )
    return await catalog.serialize_posts(session, posts, creators, creator_users, viewer)


@router.get("/creators/{handle}/posts", response_model=list[PostOut])
async def creator_posts(
    handle: str,
    session: SessionDep,
    viewer: OptionalUserDep,
    limit: int = Query(30, ge=1, le=100),
    offset: int = Query(0, ge=0),
) -> list[PostOut]:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    # The author still sees their own removed posts, flagged; nobody else does.
    visible_only = viewer is None or viewer.id != creator.user_id
    query = select(Post).where(Post.creator_id == creator.id)
    if visible_only:
        query = query.where(Post.status == "visible")
    result = await session.execute(
        query
        .order_by(Post.created_at.desc(), Post.id.desc())
        .limit(limit)
        .offset(offset)
    )
    posts = list(result.scalars().all())
    owner = await session.get(User, creator.user_id)
    return await catalog.serialize_posts(
        session, posts, {creator.id: creator}, {creator.user_id: owner}, viewer
    )


async def _post_or_404(session: SessionDep, post_id: int) -> tuple[Post, CreatorProfile]:
    post = await session.get(Post, post_id)
    if post is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such post")
    creator = await session.get(CreatorProfile, post.creator_id)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such post")
    return post, creator


@router.get("/posts/{post_id}", response_model=PostOut)
async def get_post(post_id: int, session: SessionDep, viewer: OptionalUserDep) -> PostOut:
    post, creator = await _post_or_404(session, post_id)
    owner = await session.get(User, creator.user_id)
    out = await catalog.serialize_posts(
        session, [post], {creator.id: creator}, {creator.user_id: owner}, viewer
    )
    return out[0]


@router.post("/posts/{post_id}/unlock", response_model=PurchaseResult)
async def unlock_post(post_id: int, user: UserDep, session: SessionDep) -> PurchaseResult:
    """Buy permanent access to one pay-per-view post."""
    post, creator = await _post_or_404(session, post_id)
    if creator.user_id == user.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "It's your post — you already have it")
    if post.visibility != "ppv":
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "This post is not sold separately")

    already = await session.execute(
        select(PostUnlock).where(PostUnlock.post_id == post.id, PostUnlock.user_id == user.id)
    )
    if already.scalar_one_or_none():
        return PurchaseResult(
            ok=True,
            charged_cents=0,
            wallet_balance_cents=user.wallet_balance_cents,
            detail="You already own this post",
        )

    creator_user = await session.get(User, creator.user_id)
    try:
        await payments.charge(
            session,
            payer=user,
            creator=creator,
            creator_user=creator_user,
            amount_cents=post.price_cents,
            kind="ppv",
            reference_type="post",
            reference_id=post.id,
            note=post.title[:200],
        )
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc

    session.add(PostUnlock(post_id=post.id, user_id=user.id, amount_cents=post.price_cents))
    post.unlock_count += 1
    await session.commit()
    return PurchaseResult(
        ok=True,
        charged_cents=post.price_cents,
        wallet_balance_cents=user.wallet_balance_cents,
        detail=f"Unlocked for {money(post.price_cents)}",
    )


@router.post("/posts/{post_id}/like", status_code=status.HTTP_204_NO_CONTENT)
async def like_post(post_id: int, user: UserDep, session: SessionDep) -> None:
    post, _ = await _post_or_404(session, post_id)
    existing = await session.execute(
        select(PostLike).where(PostLike.post_id == post.id, PostLike.user_id == user.id)
    )
    if existing.scalar_one_or_none() is None:
        session.add(PostLike(post_id=post.id, user_id=user.id))
        post.like_count += 1
        await session.commit()


@router.delete("/posts/{post_id}/like", status_code=status.HTTP_204_NO_CONTENT)
async def unlike_post(post_id: int, user: UserDep, session: SessionDep) -> None:
    post, _ = await _post_or_404(session, post_id)
    existing = await session.execute(
        select(PostLike).where(PostLike.post_id == post.id, PostLike.user_id == user.id)
    )
    if existing.scalar_one_or_none() is not None:
        await session.execute(
            delete(PostLike).where(PostLike.post_id == post.id, PostLike.user_id == user.id)
        )
        post.like_count = max(0, post.like_count - 1)
        await session.commit()


@router.get("/posts/{post_id}/comments", response_model=list[CommentOut])
async def list_comments(post_id: int, session: SessionDep) -> list[CommentOut]:
    await _post_or_404(session, post_id)
    result = await session.execute(
        select(Comment, User)
        .join(User, User.id == Comment.user_id)
        .where(Comment.post_id == post_id, Comment.status == "visible")
        .order_by(Comment.created_at)
    )
    return [
        CommentOut(
            id=c.id,
            user_display_name=u.display_name,
            user_avatar_emoji=u.avatar_emoji,
            body=c.body,
            created_at=c.created_at,
        )
        for c, u in result.all()
    ]


@router.post("/posts/{post_id}/comments", response_model=CommentOut, status_code=201)
async def add_comment(
    post_id: int, payload: CommentCreate, user: UserDep, session: SessionDep
) -> CommentOut:
    """Commenting needs the same access the post itself needs — no shouting through a paywall."""
    post, creator = await _post_or_404(session, post_id)
    prices = await catalog.tier_price_map(session)
    unlocked = await catalog.unlocked_post_ids(session, user)
    access = decide_post_access(
        is_owner=user.id == creator.user_id,
        visibility=post.visibility,
        price_cents=post.price_cents,
        min_tier_price_cents=prices.get(post.min_tier_id) if post.min_tier_id else None,
        viewer_tier_price_cents=await catalog.viewer_tier_price_cents(session, user, creator.id),
        has_unlock=post.id in unlocked,
        is_adult=post.is_adult or creator.is_adult_channel,
        viewer_is_verified_adult=user.age_check_status == "verified",
        is_removed=post.status == "removed",
    )
    if not access.granted:
        raise HTTPException(status.HTTP_403_FORBIDDEN, access.unlock_label or "Locked")

    comment = Comment(post_id=post.id, user_id=user.id, body=payload.body)
    session.add(comment)
    post.comment_count += 1
    await session.commit()
    await session.refresh(comment)
    return CommentOut(
        id=comment.id,
        user_display_name=user.display_name,
        user_avatar_emoji=user.avatar_emoji,
        body=comment.body,
        created_at=comment.created_at,
    )
