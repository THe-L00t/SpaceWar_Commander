#pragma once
#include <cstdint>

// ============================================================
//  Shared/Model/ModelFormat.h — 지원 포맷과 .swm 파일 규격
//
//  ★ 외부 SDK 를 쓰지 않는다 (2026-10-05 방침)
//    FBX SDK · Assimp 를 전부 뺀다. 포맷 세 가지를 직접 읽는다.
//      · .swm        우리가 만든 운영 포맷 (블렌더에서 구워 넣는다)
//      · .glb/.gltf  glTF 2.0 — 제작 포맷 그대로 반입(검증·임시 반입용)
//      · .obj        단일 소품·디버깅용
//
//  ★ 판별은 호출부가 하지 않는다
//    ModelReader 가 파일 앞부분의 매직과 확장자로 고른다. 부르는 쪽은 경로만 준다.
//
//  설계 원본: 노션 「모델 파서 직접 구현 — 설계안 v1」
// ============================================================

namespace Shared {

	enum class ModelFormat : uint32_t
	{
		Unknown = 0,
		Swm,      // 자체 바이너리
		Gltf,     // .gltf(JSON) · .glb(바이너리 컨테이너)
		Obj,      // OBJ + MTL
	};

	// 배열 인덱스의 «없음». 0 을 무효로 쓰지 않는다 — 0번 재질·0번 노드가 유효하기 때문이다.
	inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

	// ── .swm v1 규격 ────────────────────────────────────────
	//
	//  [헤더 32B] → [섹션 테이블 16B × N] → [섹션 본문…]
	//
	//  ★ 섹션 테이블 방식을 쓰는 이유
	//    모르는 섹션을 건너뛸 수 있다. 서버는 COLL 만 읽고 렌더 데이터는 건드리지 않는다.
	//    나중에 SKIN·ANIM 을 넣어도 구버전 리더가 그대로 읽는다.
	namespace swm {

		inline constexpr uint32_t kMagic = 0x314D5753u;   // 'S','W','M','1' (리틀엔디안)
		inline constexpr uint16_t kVersion = 1;

		// 헤더 flags
		inline constexpr uint16_t kFlagYUp = 1 << 0;          // Y-up 으로 변환해 구웠다
		inline constexpr uint16_t kFlagTriangulated = 1 << 1; // 삼각화까지 끝냈다
		inline constexpr uint16_t kFlagHasTangents = 1 << 2;  // 탄젠트가 들어 있다

		// 섹션 ID (4바이트 ASCII)
		inline constexpr uint32_t kSectionMesh = 0x4853454Du;   // 'MESH'
		inline constexpr uint32_t kSectionMaterial = 0x4C54414Du;// 'MATL'
		inline constexpr uint32_t kSectionNode = 0x45444F4Eu;   // 'NODE'
		inline constexpr uint32_t kSectionCollision = 0x4C4C4F43u;// 'COLL'

		// 한 파일이 가질 수 있는 상한. 부정 입력을 여기서 먼저 걸러낸다.
		inline constexpr uint32_t kMaxSections = 64;
		inline constexpr uint32_t kMaxMeshes = 4096;
		inline constexpr uint32_t kMaxMaterials = 4096;
		inline constexpr uint32_t kMaxNodes = 65536;
		inline constexpr uint32_t kMaxVertices = 16u * 1024u * 1024u;
		inline constexpr uint32_t kMaxIndices = 64u * 1024u * 1024u;
		inline constexpr uint16_t kMaxNameLength = 1024;

#pragma pack(push, 1)
		struct FileHeader
		{
			uint32_t magic;       // kMagic
			uint16_t version;     // kVersion
			uint16_t flags;       // kFlag*
			uint32_t sections;    // 섹션 테이블 항목 수
			uint8_t  reserved[20];
		};

		struct SectionEntry
		{
			uint32_t id;          // kSection*
			uint32_t size;        // 본문 바이트 수
			uint64_t offset;      // 파일 처음부터의 오프셋
		};
#pragma pack(pop)

		static_assert(sizeof(FileHeader) == 32, "swm FileHeader 크기 변경됨");
		static_assert(sizeof(SectionEntry) == 16, "swm SectionEntry 크기 변경됨");

	} // namespace swm

} // namespace Shared
