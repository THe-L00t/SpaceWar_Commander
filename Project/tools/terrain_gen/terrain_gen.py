# ============================================================
#  terrain_gen.py — 지형 조각 하이트맵 생성기
#
#  게임은 이 스크립트를 쓰지 않는다. 에셋(조각 하이트맵)을 미리 굽는 도구다.
#
#  출력 3종:
#    <name>.png            16비트 흑백 하이트맵  ← 실제 산출물
#    <name>_shade.png      음영기복 미리보기     ← 눈으로 판단용
#    <name>_slope.png      경사도 맵            ← 보행 가능 검증용 (빨강 = 못 걸음)
# ============================================================
import argparse
import numpy as np
from PIL import Image

# ── 노이즈 ────────────────────────────────────────────────

def perlin(shape, cells, seed):
    """격자 기반 그래디언트 노이즈. 반환 범위 대략 [-0.7, 0.7]."""
    rng = np.random.default_rng(seed)
    cy, cx = cells
    ang = rng.uniform(0.0, 2.0 * np.pi, (cy + 2, cx + 2))
    gx, gy = np.cos(ang), np.sin(ang)

    ys = np.linspace(0.0, cy, shape[0], endpoint=False)
    xs = np.linspace(0.0, cx, shape[1], endpoint=False)
    Y, X = np.meshgrid(ys, xs, indexing="ij")

    y0 = Y.astype(np.int32)
    x0 = X.astype(np.int32)
    fy = Y - y0
    fx = X - x0

    def corner(iy, ix, dy, dx):
        return gx[iy, ix] * dx + gy[iy, ix] * dy

    n00 = corner(y0,     x0,     fy,       fx)
    n10 = corner(y0 + 1, x0,     fy - 1.0, fx)
    n01 = corner(y0,     x0 + 1, fy,       fx - 1.0)
    n11 = corner(y0 + 1, x0 + 1, fy - 1.0, fx - 1.0)

    u = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    v = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0)

    return (n00 * (1 - u) + n01 * u) * (1 - v) + (n10 * (1 - u) + n11 * u) * v


def fbm(shape, cells, octaves, seed, lacunarity=2.0, gain=0.5, ridged=False):
    """옥타브를 겹쳐 프랙탈 노이즈. ridged=True 면 능선형."""
    total = np.zeros(shape, np.float64)
    amp = 1.0
    norm = 0.0
    c = float(cells)
    for o in range(octaves):
        n = perlin(shape, (max(1, int(round(c))), max(1, int(round(c)))), seed + o * 7919)
        if ridged:
            n = 1.0 - np.abs(n) / 0.7
            n = n * n
            n = n - 0.5
        total += n * amp
        norm += amp
        amp *= gain
        c *= lacunarity
    return total / norm


def normalize(h):
    lo, hi = h.min(), h.max()
    return (h - lo) / (hi - lo) if hi > lo else np.zeros_like(h)


def domain_warp(shape, cells, seed, strength):
    """좌표를 노이즈로 밀어 능선을 휘게 한다. (오프셋 배열 반환)"""
    wx = fbm(shape, cells, 4, seed + 1301) * strength
    wy = fbm(shape, cells, 4, seed + 5077) * strength
    return wx, wy


def sample_bilinear(field, y, x):
    H, W = field.shape
    y = np.clip(y, 0, H - 1.001)
    x = np.clip(x, 0, W - 1.001)
    y0 = y.astype(np.int32)
    x0 = x.astype(np.int32)
    fy = y - y0
    fx = x - x0
    a = field[y0, x0]
    b = field[y0, x0 + 1]
    c = field[y0 + 1, x0]
    d = field[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


# ── 침식 ──────────────────────────────────────────────────

def hydraulic_erosion(h, droplets=400_000, batch=20_000, steps=48, seed=0,
                      inertia=0.05, capacity_factor=4.0, min_capacity=0.01,
                      erode_rate=0.3, deposit_rate=0.3, evaporate=0.02,
                      gravity=4.0):
    """
    입자 기반 수력 침식. 물방울이 경사를 따라 흐르며 흙을 깎고 쌓는다.
    이 단계가 노이즈를 '지형'으로 바꾼다 — 골짜기가 파이고 선상지가 생긴다.
    """
    h = h.copy()
    H, W = h.shape
    rng = np.random.default_rng(seed)
    n_batches = max(1, droplets // batch)

    for _ in range(n_batches):
        px = rng.uniform(1.0, W - 2.0, batch)
        py = rng.uniform(1.0, H - 2.0, batch)
        dx = np.zeros(batch)
        dy = np.zeros(batch)
        speed = np.ones(batch)
        water = np.ones(batch)
        sed = np.zeros(batch)
        alive = np.ones(batch, bool)

        for _ in range(steps):
            if not alive.any():
                break

            x0 = px.astype(np.int32)
            y0 = py.astype(np.int32)
            fx = px - x0
            fy = py - y0

            h00 = h[y0, x0]
            h01 = h[y0, x0 + 1]
            h10 = h[y0 + 1, x0]
            h11 = h[y0 + 1, x0 + 1]

            gx = (h01 - h00) * (1 - fy) + (h11 - h10) * fy
            gy = (h10 - h00) * (1 - fx) + (h11 - h01) * fx
            height = (h00 * (1 - fx) + h01 * fx) * (1 - fy) + (h10 * (1 - fx) + h11 * fx) * fy

            dx = dx * inertia - gx * (1.0 - inertia)
            dy = dy * inertia - gy * (1.0 - inertia)
            mag = np.sqrt(dx * dx + dy * dy)
            flat = mag < 1e-9
            if flat.any():
                a = rng.uniform(0, 2 * np.pi, flat.sum())
                dx[flat] = np.cos(a)
                dy[flat] = np.sin(a)
                mag[flat] = 1.0
            dx /= mag
            dy /= mag

            nx = px + dx
            ny = py + dy

            out = (nx < 1) | (nx >= W - 2) | (ny < 1) | (ny >= H - 2)
            alive &= ~out
            if not alive.any():
                break
            nx = np.clip(nx, 1, W - 2.001)
            ny = np.clip(ny, 1, H - 2.001)

            new_h = sample_bilinear(h, ny, nx)
            dh = new_h - height

            cap = np.maximum(-dh * speed * water * capacity_factor, min_capacity)

            # 위로 가거나 용량 초과 → 퇴적 / 아니면 침식
            uphill = dh > 0.0
            over = sed > cap
            amount = np.where(
                uphill | over,
                -np.where(uphill, np.minimum(dh, sed), (sed - cap) * deposit_rate),
                np.minimum((cap - sed) * erode_rate, -dh),
            )
            # amount > 0 이면 침식(h 감소), < 0 이면 퇴적(h 증가)
            amount = np.where(alive, amount, 0.0)
            sed += amount

            w00 = (1 - fx) * (1 - fy)
            w01 = fx * (1 - fy)
            w10 = (1 - fx) * fy
            w11 = fx * fy
            np.add.at(h, (y0, x0), -amount * w00)
            np.add.at(h, (y0, x0 + 1), -amount * w01)
            np.add.at(h, (y0 + 1, x0), -amount * w10)
            np.add.at(h, (y0 + 1, x0 + 1), -amount * w11)

            speed = np.sqrt(np.maximum(speed * speed + (-dh) * gravity, 0.0))
            water *= (1.0 - evaporate)
            px, py = nx, ny

    return h


def thermal_erosion(h, max_slope, iters=60, rate=0.5):
    """
    최대 경사각을 넘는 사면을 무너뜨린다 (애추 사면).
    ★ 보행 가능 경사를 강제하는 수단이기도 하다.
    max_slope 는 인접 셀 간 허용 높이차(정규화 단위).
    """
    h = h.copy()
    for _ in range(iters):
        total = np.zeros_like(h)
        for ay, ax in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            d = h - np.roll(np.roll(h, ay, 0), ax, 1)
            excess = np.maximum(d - max_slope, 0.0) * rate * 0.25
            total -= excess
            total += np.roll(np.roll(excess, -ay, 0), -ax, 1)
        h += total
    return h


# ── 미리보기 ──────────────────────────────────────────────

def hillshade(h, relief_m, tile_m, azimuth=315.0, altitude=35.0):
    cell = tile_m / h.shape[0]
    z = h * relief_m
    dzdy, dzdx = np.gradient(z, cell)
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(dzdy, -dzdx)
    az = np.radians(360.0 - azimuth + 90.0)
    zen = np.radians(90.0 - altitude)
    hs = np.cos(zen) * np.cos(slope) + np.sin(zen) * np.sin(slope) * np.cos(az - aspect)
    hs = np.clip(hs, 0, 1)
    # 고도에 따른 옅은 색조를 섞어 형태를 읽기 쉽게
    tint = np.stack([0.55 + 0.45 * h, 0.60 + 0.35 * h, 0.55 + 0.30 * h], -1)
    return np.clip(hs[..., None] * tint * 255.0, 0, 255).astype(np.uint8)


def slope_map(h, relief_m, tile_m, limit_deg=45.0):
    cell = tile_m / h.shape[0]
    z = h * relief_m
    dzdy, dzdx = np.gradient(z, cell)
    deg = np.degrees(np.arctan(np.hypot(dzdx, dzdy)))
    img = np.zeros(h.shape + (3,), np.uint8)
    t = np.clip(deg / limit_deg, 0, 1)
    img[..., 1] = ((1 - t) * 220).astype(np.uint8)          # 완만 = 초록
    img[..., 0] = (t * 220).astype(np.uint8)                # 급경사 = 노랑~빨강
    over = deg > limit_deg
    img[over] = (255, 0, 0)                                  # 초과 = 순수 빨강
    return img, deg, over.mean() * 100.0


# ── 지형 종류 프리셋 ──────────────────────────────────────

def make_mountain(size, seed):
    shape = (size, size)
    wx, wy = domain_warp(shape, 3, seed, strength=0.35)
    base = fbm(shape, 3, 8, seed, ridged=True)
    detail = fbm(shape, 12, 6, seed + 991)
    # 워핑: 능선 노이즈를 밀어 흐름을 만든다
    ys, xs = np.meshgrid(np.arange(size), np.arange(size), indexing="ij")
    warped = sample_bilinear(base, ys + wy * size * 0.08, xs + wx * size * 0.08)
    h = normalize(warped * 1.0 + detail * 0.22)
    h = np.power(h, 1.25)
    return h


PRESETS = {"mountain": make_mountain}


# ── 메인 ──────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", default="mountain", choices=sorted(PRESETS))
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--tile-m", type=float, default=1000.0, help="조각이 덮는 실제 크기(m)")
    ap.add_argument("--relief-m", type=float, default=150.0, help="최고-최저 표고차(m)")
    ap.add_argument("--slope-limit", type=float, default=45.0, help="보행 가능 최대 경사(도)")
    ap.add_argument("--droplets", type=int, default=300_000)
    ap.add_argument("--out", default="out")
    args = ap.parse_args()

    print(f"[1/4] 기본 형태 생성  {args.type} {args.size}x{args.size} seed={args.seed}")
    h = PRESETS[args.type](args.size, args.seed)

    print(f"[2/4] 수력 침식  물방울 {args.droplets:,}")
    h = hydraulic_erosion(h, droplets=args.droplets, seed=args.seed)
    h = normalize(h)

    # 셀 간 허용 높이차 = tan(경사한계) * 셀크기 / 표고차
    cell = args.tile_m / args.size
    max_step = np.tan(np.radians(args.slope_limit)) * cell / args.relief_m
    print(f"[3/4] 열 침식  경사한계 {args.slope_limit}deg (셀당 허용 {max_step:.5f})")
    h = thermal_erosion(h, max_slope=max_step, iters=80)
    h = normalize(h)

    print("[4/4] 저장")
    Image.fromarray((h * 65535.0).astype(np.uint16), mode="I;16").save(f"{args.out}.png")
    Image.fromarray(hillshade(h, args.relief_m, args.tile_m)).save(f"{args.out}_shade.png")
    smap, deg, over_pct = slope_map(h, args.relief_m, args.tile_m, args.slope_limit)
    Image.fromarray(smap).save(f"{args.out}_slope.png")

    print()
    print(f"  표고차          {args.relief_m:.0f} m / {args.tile_m:.0f} m 조각")
    print(f"  셀 간격         {cell:.2f} m")
    print(f"  평균 경사       {deg.mean():.1f} deg")
    print(f"  최대 경사       {deg.max():.1f} deg")
    print(f"  한계 초과 면적  {over_pct:.2f} %  (걸을 수 없는 지역)")


if __name__ == "__main__":
    main()
