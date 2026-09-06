"""Fill a database with a small demo world so the app has something to show.

Everything it writes is invented for demonstration: the creators, their posts,
prices, and messages are not real people, real content, or real revenue. Point
it at a throwaway database, never a production one.

    python scripts/seed_demo.py            # writes to DATABASE_URL (default: local SQLite)
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ.setdefault("JWT_SECRET", "seed-script-secret-not-for-production")

from sqlalchemy import select  # noqa: E402

from app.core.db import SessionLocal, create_all  # noqa: E402
from app.core.security import hash_password  # noqa: E402
from app.models.orm import (  # noqa: E402
    CreatorProfile,
    Follow,
    LiveEvent,
    Post,
    Product,
    Subscription,
    Tier,
    User,
)

PASSWORD = "livephoria-demo"

CREATORS = [
    {
        "handle": "aaliyahrose",
        "name": "Aaliyah Rose",
        "emoji": "🎤",
        "tagline": "Singer. Rehearsals, demos, and the stuff that doesn't make the record.",
        "bio": "Recording between tour legs. Members get the rough takes before anyone else.",
        "category": "music",
        "accent": "#7c5cff",
        "tiers": [
            ("Insider", 999, ["Members-only posts", "Early demo drops"], False),
            ("Front Row", 2500, ["Everything in Insider", "Monthly VIP live", "Name in credits"], True),
        ],
        "posts": [
            ("video", "New rehearsal footage 👀", "Two hours of run-throughs before the tour.", "subscribers", 0),
            ("photo", "Sound check", "Empty room, best acoustics we've had all year.", "public", 0),
            ("audio", "Unreleased demo — 'Slow Exit'", "Rough vocal take, one mic.", "ppv", 1200),
        ],
        "live": ("Backstage with me", "ticket", 1500, "live"),
        "products": [
            ("merch", "Tour tee", "Heavyweight cotton, screen printed.", 3200, 40, None),
            ("digital", "Demo pack (6 tracks)", "WAV downloads.", 900, None, "https://example.test/demo-pack.zip"),
        ],
    },
    {
        "handle": "marcusplays",
        "name": "Marcus",
        "emoji": "🎧",
        "tagline": "Producer. Listening parties, beat breakdowns, gear talk.",
        "bio": "I take records apart on stream and put them back together badly.",
        "category": "music",
        "accent": "#ff4d8d",
        "tiers": [("Studio Pass", 799, ["Beat breakdowns", "Sample packs"], False)],
        "posts": [
            ("video", "New music listening party", "Playing the whole record start to finish.", "public", 0),
            ("text", "How the drums on track 4 were made", "Layering, then ruining it on purpose.", "subscribers", 0),
        ],
        "live": ("New music listening party", "free", 0, "live"),
        "products": [("digital", "Sample pack vol. 2", "120 one-shots.", 1500, None, "https://example.test/pack2.zip")],
    },
    {
        "handle": "jadedances",
        "name": "Jade",
        "emoji": "💃",
        "tagline": "Choreographer. Class footage, breakdowns, and the outtakes.",
        "bio": "Two classes a week, filmed end to end. Slowed breakdowns for members.",
        "category": "dance",
        "accent": "#2fd48b",
        "tiers": [
            ("Class Pass", 1200, ["Full class recordings", "Slowed breakdowns"], False),
            ("Studio VIP", 4000, ["Everything in Class Pass", "Feedback on your clips"], True),
        ],
        "posts": [
            ("video", "Full class — Friday intermediate", "Ninety minutes, four combos.", "subscribers", 0),
            ("photo", "Outtakes", "The takes where I fell over.", "public", 0),
        ],
        "live": ("Sunday warm-up", "subscribers", 0, "scheduled"),
        "products": [("merch", "Studio hoodie", "Oversized, embroidered.", 5500, 12, None)],
    },
]

FANS = [("Sam", "🙂"), ("Riley", "😎"), ("Devon", "🌟"), ("Noor", "🎬")]


async def main() -> None:
    await create_all()
    async with SessionLocal() as session:
        existing = await session.execute(select(CreatorProfile).limit(1))
        if existing.scalar_one_or_none():
            print("Database already has creators — nothing seeded.")
            return

        now = datetime.now(timezone.utc)
        creator_rows: list[tuple[CreatorProfile, list[Tier]]] = []

        for spec in CREATORS:
            user = User(
                email=f"{spec['handle']}@example.test",
                password_hash=hash_password(PASSWORD),
                display_name=spec["name"],
                avatar_emoji=spec["emoji"],
                is_creator=True,
            )
            session.add(user)
            await session.flush()

            creator = CreatorProfile(
                user_id=user.id,
                handle=spec["handle"],
                display_name=spec["name"],
                tagline=spec["tagline"],
                bio=spec["bio"],
                category=spec["category"],
                accent_color=spec["accent"],
            )
            session.add(creator)
            await session.flush()

            tiers = []
            for name, price, perks, is_vip in spec["tiers"]:
                tier = Tier(
                    creator_id=creator.id,
                    name=name,
                    price_cents=price,
                    perks=json.dumps(perks),
                    is_vip=is_vip,
                    description="",
                )
                session.add(tier)
                tiers.append(tier)
            await session.flush()

            for index, (kind, title, body, visibility, price) in enumerate(spec["posts"]):
                session.add(
                    Post(
                        creator_id=creator.id,
                        kind=kind,
                        title=title,
                        body=body,
                        visibility=visibility,
                        price_cents=price,
                        created_at=now - timedelta(hours=index * 7 + 1),
                        like_count=0,
                    )
                )

            title, access, ticket, status = spec["live"]
            session.add(
                LiveEvent(
                    creator_id=creator.id,
                    title=title,
                    description="",
                    access=access,
                    ticket_price_cents=ticket,
                    status=status,
                    started_at=now if status == "live" else None,
                    scheduled_for=None if status == "live" else now + timedelta(days=2),
                )
            )

            for kind, name, description, price, inventory, asset in spec["products"]:
                session.add(
                    Product(
                        creator_id=creator.id,
                        kind=kind,
                        name=name,
                        description=description,
                        price_cents=price,
                        inventory=inventory,
                        digital_asset_url=asset,
                    )
                )
            creator_rows.append((creator, tiers))

        for index, (name, emoji) in enumerate(FANS):
            fan = User(
                email=f"{name.lower()}@example.test",
                password_hash=hash_password(PASSWORD),
                display_name=name,
                avatar_emoji=emoji,
                wallet_balance_cents=10_000,
            )
            session.add(fan)
            await session.flush()
            # Each fan follows everyone and subscribes to one creator's entry tier.
            for creator, tiers in creator_rows:
                session.add(Follow(fan_id=fan.id, creator_id=creator.id))
            creator, tiers = creator_rows[index % len(creator_rows)]
            session.add(
                Subscription(
                    fan_id=fan.id,
                    creator_id=creator.id,
                    tier_id=tiers[0].id,
                    current_period_end=now + timedelta(days=30),
                )
            )

        await session.commit()

    print("Seeded demo data (fictional).")
    print(f"  Creators: {', '.join('@' + c['handle'] for c in CREATORS)}")
    print(f"  Fans:     {', '.join(f[0].lower() + '@example.test' for f in FANS)}")
    print(f"  Password for every demo account: {PASSWORD}")


if __name__ == "__main__":
    asyncio.run(main())
