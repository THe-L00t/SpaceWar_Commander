# ============================================================
#  make_terrain.py — Gaea .terrain 그래프 파일 생성기
#
#  Gaea 예제 파일은 (1) 출력 노드가 지정돼 있지 않고 (2) 구버전이라
#  Swarm CLI 로 빌드하면 아무것도 나오지 않는다. 그래서 직접 만든다.
#
#  출력 노드에 SaveDefinition(Format=PNG16)을 붙이므로 헤드리스 빌드가 된다.
#
#  사용:  python make_terrain.py --out <폴더> [--types mountain,canyon] [--seeds 8]
# ============================================================
import argparse
import json
import os
import uuid

# ── 노드별 포트 정의 (Gaea 예제에서 추출한 실제 구조) ──────────
GEN = [("In", "PrimaryIn"), ("Out", "PrimaryOut")]
REQ = [("In", "PrimaryIn, Required"), ("Out", "PrimaryOut")]

PORTS = {
    "Mountain":      GEN,
    "MountainRange": GEN,
    "Canyon":        GEN + [("Depth", "Out")],
    "Crater":        GEN,
    "CraterField":   GEN,
    "Craggy":        GEN,
    "Ridge":         GEN,
    "Perlin":        GEN,
    "Plates":        GEN,
    "Rugged":        GEN,
    "Stratify":      REQ + [("Layers", "Out")],
    "Sandstone":     REQ + [("Layers", "Out")],
    "Terraces":      REQ + [("Modulation", "In")],
    "Outcrops":      REQ,
    "Erosion2":      REQ + [("Flow", "Out"), ("Wear", "Out"), ("Deposits", "Out")],
    "Autolevel":     REQ,
    # 침식 대체 후보
    "Erosion":       REQ + [("AreaMask", "In"), ("SedimentRemoval", "In"),
                            ("Wear", "Out"), ("Deposits", "Out"), ("Flow", "Out")],
    "Thermal2":      REQ + [("AreaMask", "In"), ("SedimentRemoval", "In"),
                            ("Wear", "Out"), ("Deposits", "Out")],
    "Sediments":     REQ + [("Sediments", "Out"), ("Deposits", "Out")],
    "HydroFix":      REQ,
    "Shaper":        REQ,
    "Distress":      REQ,
    "Crumble":       REQ + [("AreaMask", "In"), ("Wear", "Out")],
}

# Seed 파라미터를 갖는 노드
SEEDED = {"Mountain", "MountainRange", "Canyon", "Crater", "CraterField", "Craggy",
          "Ridge", "Perlin", "Plates", "Rugged", "Stratify", "Sandstone",
          "Terraces", "Outcrops", "Erosion2", "Erosion", "Distress"}

# ── 지형 종류별 노드 체인 ────────────────────────────────────
#   마지막 노드가 출력(SaveDefinition)이 붙는 노드다.
CHAINS = {
    "mountain": [("MountainRange", {}), ("Erosion2", {}), ("Autolevel", {})],
    "peaks":    [("Mountain", {}), ("Erosion2", {}), ("Autolevel", {})],
    "canyon":   [("Canyon", {"Valley": 0.6, "Depth": 1.0}), ("Stratify", {}),
                 ("Erosion2", {}), ("Autolevel", {})],
    "crater":   [("CraterField", {"Scale": 3.0, "Depth": 1.2, "Density": 1.0}),
                 ("Craggy", {"Size": 2.0, "Depth": 0.6}), ("Erosion2", {}), ("Autolevel", {})],
    "plateau":  [("Plates", {}), ("Stratify", {}),
                 ("Terraces", {"NumTerraces": 6, "Uniformity": 1.0, "Steepness": 1.0}),
                 ("Erosion2", {}), ("Autolevel", {})],
    "badlands": [("Ridge", {"Height": 1.4, "Definition": 0.4}),
                 ("Sandstone", {"Passes": 2, "Spacing": 0.32, "Chaos": 0.4}),
                 ("Erosion2", {}), ("Autolevel", {})],
    "plains":   [("Perlin", {"Frequency": 0.05, "Gain": 0.35, "Clamp": 0.5}),
                 ("Erosion2", {}), ("Autolevel", {})],
    "rugged":   [("Rugged", {}), ("Outcrops", {}), ("Erosion2", {}), ("Autolevel", {})],
}


class Builder:
    """Newtonsoft.Json 의 $id 는 직렬화 순서대로 매겨진다. 순서대로 만든다."""

    def __init__(self):
        self.counter = 0

    def nid(self):
        self.counter += 1
        return str(self.counter)

    def obj(self, **kw):
        d = {"$id": self.nid()}
        d.update(kw)
        return d


def build(type_name, seed, tile_m, relief_m, resolution, out_name):
    if type_name not in CHAINS:
        raise ValueError(f"알 수 없는 지형 종류: {type_name}")

    b = Builder()
    chain = CHAINS[type_name]

    root_id = b.nid()                      # 1
    assets_id = b.nid()                    # 2
    asset_id = b.nid()                     # 3
    terrain_id = b.nid()                   # 4
    meta_id = b.nid()                      # 5

    stamp = "2026-07-31 00:00:00Z"
    terrain_meta = {
        "$id": meta_id, "Name": "", "Description": "",
        "Version": "2.3.0.1",
        "DateCreated": stamp, "DateLastBuilt": stamp, "DateLastSaved": stamp,
        "ModifiedVersion": "2.3.0.1",
    }

    nodes_container_id = b.nid()           # 6
    nodes = {"$id": nodes_container_id}

    node_ids = [100 + i * 10 for i in range(len(chain))]
    node_refs = {}

    for idx, (ntype, params) in enumerate(chain):
        nid_num = node_ids[idx]
        node_ref = b.nid()
        node_refs[nid_num] = node_ref

        node = {"$id": node_ref, "$type": f"QuadSpinner.Gaea.Nodes.{ntype}, Gaea.Nodes"}
        node.update(params)
        if ntype in SEEDED:
            node["Seed"] = (seed * 7919 + idx * 104729) % 65536

        node["Id"] = nid_num
        node["Name"] = ntype
        node["Position"] = b.obj(X=25000.0 + idx * 300.0, Y=25000.0)

        # 마지막 노드에만 출력 지정
        if idx == len(chain) - 1:
            node["SaveDefinition"] = {
                "$id": b.nid(),
                "Node": nid_num,
                "Filename": out_name,
                "Format": "PNG16",
                "IsEnabled": True,
                "DisabledInProfiles": b.obj(**{"$values": []}),
            }

        ports_id = b.nid()
        port_values = []
        for pname, ptype in PORTS[ntype]:
            p = {"$id": b.nid(), "Name": pname, "Type": ptype}
            if pname == "In" and idx > 0:
                p["Record"] = {
                    "$id": b.nid(),
                    "From": node_ids[idx - 1],
                    "To": nid_num,
                    "FromPort": "Out",
                    "ToPort": "In",
                    "IsValid": True,
                }
            p["IsExporting"] = True
            p["Parent"] = {"$ref": node_ref}
            port_values.append(p)
        node["Ports"] = {"$id": ports_id, "$values": port_values}
        node["Modifiers"] = b.obj(**{"$values": []})

        nodes[str(nid_num)] = node

    groups = b.obj()
    notes = b.obj()
    tabs_id = b.nid()
    tab = {"$id": b.nid(), "Name": "Graph 1", "Color": "Brass", "ZoomFactor": 1.0}
    tab["ViewportLocation"] = b.obj(X=24800.0, Y=24800.0)
    graph_tabs = {"$id": tabs_id, "$values": [tab]}

    terrain = {
        "$id": terrain_id,
        "Id": str(uuid.uuid4()),
        "Metadata": terrain_meta,
        "Nodes": nodes,
        "Groups": groups,
        "Notes": notes,
        "GraphTabs": graph_tabs,
        "Width": float(tile_m),        # 조각이 덮는 실제 크기 (m)
        "Height": float(relief_m),     # 표고차 (m)
        "Ratio": float(relief_m) / float(tile_m),
    }

    automation = {
        "$id": b.nid(),
        "Bindings": b.obj(**{"$values": []}),
        "Expressions": b.obj(),
        "VariablesEx": b.obj(),
        "Variables": b.obj(),
    }

    build_def = {
        "$id": b.nid(),
        "Resolution": resolution,
        "BakeResolution": resolution,
        "TileResolution": resolution,
        "BucketResolution": resolution,
        "NumberOfTiles": 1,
        "EdgeBlending": 0.25,
        "TileZeroIndex": True,
        "TilePattern": "_y%Y%_x%X%",
        "OrganizeFiles": "NodeSubFolder",
    }

    state_id = b.nid()
    bookmarks = b.obj(**{"$values": []})
    viewport = {"$id": b.nid(), "Camera": b.obj(),
                "RenderMode": "Realistic", "AmbientOcclusion": True, "Shadows": True}
    state = {
        "$id": state_id,
        "BakeResolution": resolution,
        "PreviewResolution": min(resolution, 1024),
        "SelectedNode": node_ids[-1],
        "NodeBookmarks": bookmarks,
        "Viewport": viewport,
    }

    asset = {"$id": asset_id, "Terrain": terrain, "Automation": automation,
             "BuildDefinition": build_def, "State": state}

    doc_meta = {
        "$id": b.nid(), "Name": "", "Description": "", "Version": "2.3.0.1", "Owner": "",
        "DateCreated": stamp, "DateLastBuilt": stamp, "DateLastSaved": stamp,
        "ModifiedVersion": "2.3.0.1",
    }

    return {
        "$id": root_id,
        "Assets": {"$id": assets_id, "$values": [asset]},
        "Id": uuid.uuid4().hex[:8],
        "Branch": 1,
        "Metadata": doc_meta,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="생성한 .terrain 을 둘 폴더")
    ap.add_argument("--types", default="", help="쉼표 구분. 비우면 전체")
    ap.add_argument("--chain", default="", help="임시 체인 직접 지정. 예: MountainRange,Erosion,Autolevel")
    ap.add_argument("--chain-name", default="adhoc", help="--chain 사용 시 이름")
    ap.add_argument("--seeds", type=int, default=8, help="종류당 시드 개수")
    ap.add_argument("--seed-start", type=int, default=1)
    ap.add_argument("--tile-m", type=float, default=1000.0)
    ap.add_argument("--relief-m", type=float, default=150.0)
    ap.add_argument("--resolution", type=int, default=1024)
    args = ap.parse_args()

    if args.chain:
        CHAINS[args.chain_name] = [(n.strip(), {}) for n in args.chain.split(",") if n.strip()]
        types = [args.chain_name]
    else:
        types = [t.strip() for t in args.types.split(",") if t.strip()] or sorted(CHAINS)
    os.makedirs(args.out, exist_ok=True)

    count = 0
    for t in types:
        for s in range(args.seed_start, args.seed_start + args.seeds):
            name = f"{t}_{s:02d}"
            doc = build(t, s, args.tile_m, args.relief_m, args.resolution, name)
            path = os.path.join(args.out, name + ".terrain")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=2, ensure_ascii=False)
            count += 1

    print(f"{count} 개 생성  ->  {args.out}")
    print(f"  종류 {len(types)}: {', '.join(types)}")
    print(f"  조각 {args.tile_m:.0f}m / 표고차 {args.relief_m:.0f}m / 해상도 {args.resolution}")


if __name__ == "__main__":
    main()
