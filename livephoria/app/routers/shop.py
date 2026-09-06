"""Creator commerce: merch and digital downloads."""
from __future__ import annotations

import secrets

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import select

from app.core.deps import CreatorDep, SessionDep, UserDep
from app.models.orm import CreatorProfile, Order, Product, User
from app.models.schemas import BuyRequest, OrderOut, ProductCreate, ProductOut
from app.services import catalog, payments

router = APIRouter(prefix="/api/v1", tags=["shop"])


def _product_out(product: Product, handle: str) -> ProductOut:
    return ProductOut(
        id=product.id,
        creator_handle=handle,
        kind=product.kind,
        name=product.name,
        description=product.description,
        image_url=product.image_url,
        price_cents=product.price_cents,
        inventory=product.inventory,
        is_active=product.is_active,
        sold_out=product.inventory is not None and product.inventory <= 0,
    )


@router.post("/creators/me/products", response_model=ProductOut, status_code=201)
async def create_product(
    payload: ProductCreate, creator: CreatorDep, session: SessionDep
) -> ProductOut:
    if payload.kind == "digital" and not payload.digital_asset_url:
        raise HTTPException(
            status.HTTP_422_UNPROCESSABLE_ENTITY, "A digital product needs a file to deliver"
        )
    if payload.kind == "digital" and payload.inventory is not None:
        raise HTTPException(
            status.HTTP_422_UNPROCESSABLE_ENTITY, "Digital products don't have inventory"
        )

    product = Product(creator_id=creator.id, **payload.model_dump())
    session.add(product)
    await session.commit()
    await session.refresh(product)
    return _product_out(product, creator.handle)


@router.get("/creators/{handle}/products", response_model=list[ProductOut])
async def list_products(handle: str, session: SessionDep) -> list[ProductOut]:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    result = await session.execute(
        select(Product)
        .where(Product.creator_id == creator.id, Product.is_active.is_(True))
        .order_by(Product.created_at.desc())
    )
    return [_product_out(p, creator.handle) for p in result.scalars().all()]


@router.post("/products/{product_id}/buy", response_model=OrderOut, status_code=201)
async def buy_product(
    product_id: int, payload: BuyRequest, user: UserDep, session: SessionDep
) -> OrderOut:
    product = await session.get(Product, product_id)
    if product is None or not product.is_active:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "That product isn't available")

    creator = await session.get(CreatorProfile, product.creator_id)
    if creator.user_id == user.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "It's your own product")

    quantity = 1 if product.kind == "digital" else payload.quantity
    if product.inventory is not None and product.inventory < quantity:
        raise HTTPException(
            status.HTTP_409_CONFLICT,
            f"Only {product.inventory} left" if product.inventory else "Sold out",
        )
    if product.kind == "merch" and not payload.shipping_address:
        raise HTTPException(
            status.HTTP_422_UNPROCESSABLE_ENTITY, "Merch needs a shipping address"
        )

    amount = product.price_cents * quantity
    creator_user = await session.get(User, creator.user_id)
    try:
        await payments.charge(
            session,
            payer=user,
            creator=creator,
            creator_user=creator_user,
            amount_cents=amount,
            kind="product",
            reference_type="product",
            reference_id=product.id,
            note=product.name[:200],
        )
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc

    if product.inventory is not None:
        product.inventory -= quantity

    order = Order(
        product_id=product.id,
        buyer_id=user.id,
        creator_id=creator.id,
        quantity=quantity,
        amount_cents=amount,
        status="paid" if product.kind == "digital" else "awaiting_fulfilment",
        shipping_address=payload.shipping_address,
        download_token=secrets.token_urlsafe(24) if product.kind == "digital" else None,
    )
    session.add(order)
    await session.commit()
    await session.refresh(order)
    return _order_out(order, product, creator.handle)


def _order_out(order: Order, product: Product, handle: str) -> OrderOut:
    return OrderOut(
        id=order.id,
        product_name=product.name,
        creator_handle=handle,
        kind=product.kind,
        quantity=order.quantity,
        amount_cents=order.amount_cents,
        status=order.status,
        created_at=order.created_at,
        download_url=f"/api/v1/orders/{order.id}/download" if order.download_token else None,
    )


@router.get("/me/orders", response_model=list[OrderOut])
async def my_orders(user: UserDep, session: SessionDep) -> list[OrderOut]:
    result = await session.execute(
        select(Order, Product, CreatorProfile)
        .join(Product, Product.id == Order.product_id)
        .join(CreatorProfile, CreatorProfile.id == Order.creator_id)
        .where(Order.buyer_id == user.id)
        .order_by(Order.created_at.desc())
    )
    return [_order_out(o, p, c.handle) for o, p, c in result.all()]


@router.get("/orders/{order_id}/download")
async def download(order_id: int, user: UserDep, session: SessionDep) -> dict:
    """Hands back the asset URL for a digital order the caller actually bought."""
    order = await session.get(Order, order_id)
    if order is None or order.buyer_id != user.id or not order.download_token:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No download for that order")
    product = await session.get(Product, order.product_id)
    return {"url": product.digital_asset_url, "product": product.name}
