# ============================================================
#  inspect_tiles.py — 하이트맵 조각 검사
#
#  Gaea 로 뽑은 PNG 들이 우리 규격에 맞는지 확인하고, 눈으로 고를 수 있게
#  음영기복 대조표(contact sheet)를 만든다.
#
#    - 16비트인지 (8비트면 계단 생김)
#    - 크기가 통일돼 있는지
#    - 실제 표고차와 경사 분포 (45도 초과 = 못 걷는 지역)
# ============================================================
import argparse
import glob
import os

import numpy as np
from PIL import Image


def load_height(path):
    im = Image.open(path)
    a = np.array(im)
    if a.ndim == 3:                      # RGB 로 나온 경우 첫 채널만
        a = a[..., 0]
    bits = 16 if a.dtype == np.uint16 else 8
    h = a.astype(np.float64) / (65535.0 if bits == 16 else 255.0)
    return h, bits, im.size


def slope_stats(h, tile_m, relief_m, limit_deg):
    cell = tile_m / max(h.shape[0] - 1, 1)
    z = h * relief_m
    dzdy, dzdx = np.gradient(z, cell)
    deg = np.degrees(np.arctan(np.hypot(dzdx, dzdy)))
    return deg.mean(), deg.max(), float((deg > limit_deg).mean() * 100.0)


def hillshade(h, tile_m, relief_m, azimuth=315.0, altitude=35.0):
    cell = tile_m / max(h.shape[0] - 1, 1)
    z = h * relief_m
    dzdy, dzdx = np.gradient(z, cell)
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(dzdy, -dzdx)
    az = np.radians(360.0 - azimuth + 90.0)
    zen = np.radians(90.0 - altitude)
    hs = np.clip(np.cos(zen) * np.cos(slope) + np.sin(zen) * np.sin(slope) * np.cos(az - aspect), 0, 1)
    tint = np.stack([0.55 + 0.45 * h, 0.60 + 0.35 * h, 0.55 + 0.30 * h], -1)
    return np.clip(hs[..., None] * tint * 255.0, 0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="PNG 들이 있는 폴더 (하위 폴더 포함)")
    ap.add_argument("--sheet", default="", help="대조표 PNG 경로 (비우면 안 만듦)")
    ap.add_argument("--tile-m", type=float, default=1000.0)
    ap.add_argument("--relief-m", type=float, default=150.0)
    ap.add_argument("--slope-limit", type=float, default=45.0)
    ap.add_argument("--thumb", type=int, default=256)
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--min-std", type=float, default=0.0,
                    help="정규화 높이 표준편차 하한. 이보다 평평하면 제외 (0.04 권장)")
    ap.add_argument("--copy-to", default="", help="통과한 조각을 복사할 폴더")
    ap.add_argument("--only-out", action="store_true", help="_Out.png 만 검사 (마스크 제외)")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.src, "**", "*.png"), recursive=True))
    if args.only_out:
        # Gaea 는 노드의 모든 출력 포트를 내보낸다. 높이는 _Out 뿐이고
        # _Flow / _Wear / _Deposits / _Layers 는 마스크다.
        paths = [p for p in paths if os.path.splitext(os.path.basename(p))[0].endswith("_Out")]
    if not paths:
        print("PNG 없음")
        return

    print(f"{'이름':<46}{'비트':<6}{'표준편차':>9}{'평균경사':>9}{'최대':>8}{'초과%':>8}  판정")
    print("-" * 100)

    thumbs = []
    kept = []
    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        parent = os.path.basename(os.path.dirname(p))
        label = f"{parent}__{name}" if parent and parent != os.path.basename(args.src) else name
        try:
            h, bits, size = load_height(p)
        except Exception as e:
            print(f"{label[:45]:<46}읽기 실패: {e}")
            continue

        std = float(h.std())
        mean_d, max_d, over = slope_stats(h, args.tile_m, args.relief_m, args.slope_limit)

        verdict = "OK"
        if bits != 16:
            verdict = "제외(8비트)"
        elif std < args.min_std:
            verdict = "제외(평평)"

        print(f"{label[:45]:<46}{bits:<6}{std:>9.4f}{mean_d:>8.1f}°{max_d:>7.1f}°{over:>7.2f}%  {verdict}")

        if verdict != "OK":
            continue
        kept.append((label, p))

        img = Image.fromarray(hillshade(h, args.tile_m, args.relief_m))
        img.thumbnail((args.thumb, args.thumb))
        thumbs.append((label, img))

    print(f"\n검사 {len(paths)} 장 -> 통과 {len(kept)} 장")

    if args.copy_to and kept:
        os.makedirs(args.copy_to, exist_ok=True)
        import shutil
        for label, src in kept:
            shutil.copy2(src, os.path.join(args.copy_to, label + ".png"))
        print(f"복사 -> {args.copy_to}")

    if args.sheet and thumbs:
        cols = args.cols
        rows = (len(thumbs) + cols - 1) // cols
        tw = max(t[1].width for t in thumbs)
        th = max(t[1].height for t in thumbs)
        pad, label_h = 6, 14
        sheet = Image.new("RGB", (cols * (tw + pad) + pad,
                                  rows * (th + pad + label_h) + pad), (24, 24, 28))
        from PIL import ImageDraw
        d = ImageDraw.Draw(sheet)
        for i, (label, img) in enumerate(thumbs):
            x = pad + (i % cols) * (tw + pad)
            y = pad + (i // cols) * (th + pad + label_h)
            sheet.paste(img, (x, y))
            d.text((x + 2, y + th + 2), label[:34], fill=(190, 190, 200))
        sheet.save(args.sheet)
        print(f"대조표 -> {args.sheet}  ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
