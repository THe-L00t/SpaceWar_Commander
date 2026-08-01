# ============================================================
#  add_output.py — Gaea 예제 .terrain 에 하이트맵 출력 노드를 붙인다
#
#  Gaea 기본 예제는 뷰포트용이라 출력(SaveDefinition)이 지정돼 있지 않아
#  Swarm CLI 로 빌드해도 아무 파일이 안 나온다. 그래서 자동으로 붙인다.
#
#  - 그래프의 종단 노드(다른 노드로 나가지 않는 노드) 중 "높이" 노드를 고른다
#  - 컬러/텍스처 계열 노드는 제외한다
#  - SaveDefinition(Format=PNG16) 을 붙이고 지형 크기/표고차를 설정한다
# ============================================================
import argparse
import collections
import glob
import json
import os

# 높이가 아니라 색/텍스처를 내보내는 노드 (출력 대상에서 제외)
COLOR_NODES = {
    "SatMap", "ColorErosion", "SuperColor", "Mixer", "Tint", "HSL", "RGBMerge",
    "RGBSplit", "WaterColor", "Shade", "Cartography", "GroundTexture", "Texturizer",
    "TextureBase", "RockMap", "Trees", "Snow", "Snowfield", "Splat", "LightX",
    "WaveShine", "Sea", "Lake", "Glacier", "DataExtractor", "FlowMap", "Mask",
    "Compare", "Threshold", "Curvature", "Distance", "Slope", "Route", "Switch",
    "LoopBegin", "LoopEnd", "Draw", "File", "Debris", "Scree", "Rockscape",
}


def node_type(n):
    return n["$type"].split(",")[0].split(".")[-1]


def load(path):
    return json.load(open(path, encoding="utf-8-sig"),
                     object_pairs_hook=collections.OrderedDict)


def pick_output_node(nodes):
    """
    높이 사슬의 마지막 노드를 고른다.

      1. 주 연결(Out -> In)만 그래프로 삼는다. Layers/Flow/Mask 같은 보조 출력은 뺀다.
      2. 컬러/텍스처 노드를 그래프에서 잘라낸다.
      3. 남은 그래프에서 가장 깊은(=사슬 끝) 노드를 고른다.

    예) Mesa: Mountain -> Erosion2 -> Sandstone -> TextureBase -> Combine -> SatMap
        컬러(TextureBase/SatMap) 제거 후 가장 깊은 노드 = Sandstone
    """
    types = {}
    for k, n in nodes.items():
        if k == "$id" or not isinstance(n, dict) or "$type" not in n:
            continue
        types[n["Id"]] = (node_type(n), n)

    alive = {nid for nid, (t, _) in types.items() if t not in COLOR_NODES}
    if not alive:
        return None

    edges = []
    for nid, (t, n) in types.items():
        for p in n.get("Ports", {}).get("$values", []):
            rec = p.get("Record")
            if not rec:
                continue
            if rec.get("FromPort") != "Out" or rec.get("ToPort") != "In":
                continue                      # 주 연결만
            f, to = rec["From"], rec["To"]
            if f in alive and to in alive:
                edges.append((f, to))

    incoming = collections.defaultdict(list)
    for f, to in edges:
        incoming[to].append(f)

    depth_cache = {}

    def depth(nid, seen=None):
        if nid in depth_cache:
            return depth_cache[nid]
        seen = seen or set()
        if nid in seen:
            return 0
        seen = seen | {nid}
        d = 0
        for f in incoming.get(nid, []):
            d = max(d, depth(f, seen) + 1)
        depth_cache[nid] = d
        return d

    best = max(alive, key=lambda nid: (depth(nid), nid))
    t, n = types[best]
    return best, t, n


def next_id(doc):
    """문서에서 사용 중인 최대 $id + 1"""
    mx = [0]

    def walk(o):
        if isinstance(o, dict):
            if "$id" in o:
                try:
                    mx[0] = max(mx[0], int(o["$id"]))
                except ValueError:
                    pass
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(doc)
    return mx[0] + 1


def process(src, dst, out_name, tile_m, relief_m, resolution):
    doc = load(src)
    asset = doc["Assets"]["$values"][0]
    terrain = asset["Terrain"]
    nodes = terrain["Nodes"]

    picked = pick_output_node(nodes)
    if picked is None:
        return None, "출력할 높이 노드를 못 찾음"

    nid, ntype, node = picked
    counter = next_id(doc)

    node["SaveDefinition"] = collections.OrderedDict([
        ("$id", str(counter)),
        ("Node", nid),
        ("Filename", out_name),
        ("Format", "PNG16"),
        ("IsEnabled", True),
        ("DisabledInProfiles", collections.OrderedDict([
            ("$id", str(counter + 1)), ("$values", []),
        ])),
    ])

    if tile_m:
        terrain["Width"] = float(tile_m)
    if relief_m:
        terrain["Height"] = float(relief_m)
    if tile_m and relief_m:
        terrain["Ratio"] = float(relief_m) / float(tile_m)
    if resolution:
        bd = asset["BuildDefinition"]
        bd["Resolution"] = resolution
        bd["BakeResolution"] = resolution

    json.dump(doc, open(dst, "w", encoding="utf-8"), indent=2, ensure_ascii=False)
    return (nid, ntype), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=r"V:\User\Gaea 2.0\Examples", help="예제 폴더 또는 단일 파일")
    ap.add_argument("--out", required=True, help="출력 .terrain 폴더")
    ap.add_argument("--tile-m", type=float, default=0, help="0이면 원본 유지")
    ap.add_argument("--relief-m", type=float, default=0)
    ap.add_argument("--resolution", type=int, default=0)
    args = ap.parse_args()

    srcs = [args.src] if os.path.isfile(args.src) else sorted(glob.glob(os.path.join(args.src, "*.terrain")))
    os.makedirs(args.out, exist_ok=True)

    ok = fail = 0
    for s in srcs:
        base = os.path.splitext(os.path.basename(s))[0]
        safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in base)
        dst = os.path.join(args.out, safe + ".terrain")
        try:
            picked, err = process(s, dst, safe, args.tile_m, args.relief_m, args.resolution)
        except Exception as e:
            picked, err = None, f"{type(e).__name__}: {e}"
        if err:
            print(f"  SKIP {base}  ({err})")
            fail += 1
        else:
            print(f"  OK   {safe:<44} 출력노드 [{picked[0]}] {picked[1]}")
            ok += 1

    print(f"\n성공 {ok} / 실패 {fail}  ->  {args.out}")


if __name__ == "__main__":
    main()
