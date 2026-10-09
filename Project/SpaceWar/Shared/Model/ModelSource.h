#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../Vec3.h"
#include "ModelFormat.h"

// ============================================================
//  Shared/Model/ModelSource.h — 포맷 중립 중간 표현
//
//  리더(glTF·OBJ)가 모두 이 구조를 채운다. 그 뒤 단계는 포맷을 모른다.
//
//  ★ 리소스가 세 종류로 갈라진다 (명세 §4 · §6.4.2)
//    명세는 Resource Manager 의 관리 대상을 «Mesh · Texture · Animation …» 으로 적고,
//    GameObject 는 «Mesh Index» 와 «Animation Index» 를 **따로** 들고 있다(6.4.2).
//    즉 메시 / 스켈레톤(본) / 애니메이션 클립은 서로 다른 리소스다. 파일 한 개에서 셋이
//    같이 나올 수도 있고(캐릭터), 클립만 든 파일이 따로 올 수도 있다(동작 하나당 한 파일).
//    그래서 이 구조도 셋을 나란히 들고, Client 쪽에서 각각 다른 리소스로 적재한다.
//
//  ★ 왜 Shared 인가 — 서버가 충돌·높이를 읽어야 한다
//    교수님 2026-09-29 지시: «높이 정보와 장애물 정보를 서버에서 가지고 NPC 이동에 활용».
//    판정은 서버 권위(명세 §18 원칙 4)다. 애니메이션은 클라 전용이므로(§14 Animator)
//    서버는 ReadOptions 로 끄고 읽는다.
//
//  ★ DirectXMath 를 쓰지 않는다
//    Shared 가 DirectX 를 물면 서버가 끌려온다. 렌더용 변환은 Client/Resource/ModelBuilder 다.
//
//  ★ 텍스처 픽셀을 담지 않는다
//    경로만 들고 있고, 읽기는 Client 의 TextureLoader(WIC) 가 한다.
// ============================================================

namespace Shared {

	// Client 의 swc::Vertex + 스킨 속성. 스킨은 별도 배열로 올라간다(지형 정점이 무거워지지 않게).
	struct SourceVertex
	{
		Vec3 position{};
		Vec3 normal{};
		Vec3 color{ 1.0f, 1.0f, 1.0f };
		Vec2 uv{};
		Vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };   // xyz = 접선, w = 종법선 부호

		// 스키닝 — glTF JOINTS_0 / WEIGHTS_0. 스킨이 없는 메시는 전부 0 이다.
		// weights 합은 리더에서 1 로 정규화한다.
		std::array<uint16_t, kJointsPerVertex> joints{};
		std::array<float, kJointsPerVertex>    weights{};
	};

	struct SourceMesh
	{
		std::vector<SourceVertex> vertices;
		std::vector<uint32_t>     indices;        // 삼각형 목록 (후처리에서 보장)
		uint32_t                  material = kInvalidIndex;
		bool                      skinned = false;   // joints/weights 가 의미 있는가
	};

	// 텍스처 슬롯 순서는 셰이더의 t1~t5 와 같아야 한다 (Client 의 TextureSlot 과 동일).
	enum class SourceTextureSlot : size_t
	{
		BaseColor, Normal, Roughness, Metallic, Emissive, Count
	};
	inline constexpr size_t kSourceTextureCount = static_cast<size_t>(SourceTextureSlot::Count);

	struct SourceMaterial
	{
		std::string name;
		Vec4  baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		Vec3  emissive{};
		float roughness = 0.65f;
		float metallic = 0.0f;

		// 모델 파일이 있는 폴더 기준 상대 경로. 빈 문자열이면 그 슬롯은 없다.
		// glTF 의 내장 이미지(.glb buffer / base64)는 ModelBuilder 가 쓸 수 있도록
		// 아래 embeddedImage 로 따로 들어온다.
		std::array<std::string, kSourceTextureCount> texturePaths;

		// 내장 이미지 인덱스 (ModelSource::images). 없으면 kInvalidIndex.
		std::array<uint32_t, kSourceTextureCount> embeddedImages{
			kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex, kInvalidIndex };
	};

	// .glb 안에 들어 있던 이미지 바이트. 디코딩(PNG/JPG → RGBA8)은 Client 의 WIC 가 한다.
	struct SourceImage
	{
		std::string          name;
		std::string          mimeType;   // "image/png" 등
		std::vector<uint8_t> bytes;
	};

	struct SourceNode
	{
		std::string name;
		uint32_t    parent = kInvalidIndex;   // ★ 부모가 반드시 자기보다 앞에 온다
		Mat4        local{};
		std::vector<uint32_t> meshes;
	};

	// ── 스켈레톤(본) — 명세 §4 의 독립 리소스 ─────────
	struct SourceJoint
	{
		std::string name;
		uint32_t    parent = kInvalidIndex;   // ★ 부모가 먼저 온다
		Mat4        localRest{};              // 바인드 자세의 로컬 변환
		Mat4        inverseBind{};            // 역바인드 행렬 (스키닝에 쓴다)
	};

	struct SkeletonSource
	{
		std::string              name;
		std::vector<SourceJoint> joints;

		bool Empty() const { return joints.empty(); }
	};

	// ── 애니메이션 클립 — 명세 §4 의 독립 리소스 ──────
	//  채널은 «조인트 하나의 한 성분(T/R/S)» 시간열이다. Animator(§14)가 샘플링해 포즈를 만든다.
	enum class AnimationPath : uint8_t { Translation, Rotation, Scale };

	// CUBICSPLINE 은 리더에서 값만 뽑아 Linear 로 낮춘다(키프레임당 값 3개 구조를 들이지 않는다).
	enum class AnimationInterpolation : uint8_t { Linear, Step };

	struct AnimationChannel
	{
		uint32_t               joint = kInvalidIndex;    // SkeletonSource::joints 인덱스
		AnimationPath          path = AnimationPath::Translation;
		AnimationInterpolation interpolation = AnimationInterpolation::Linear;

		std::vector<float> times;    // 초. 오름차순
		std::vector<float> values;   // T·S = 3개씩, R = 4개씩(쿼터니언 xyzw)
	};

	struct AnimationSource
	{
		std::string                   name;
		float                         duration = 0.0f;   // 초
		std::vector<AnimationChannel> channels;
	};

	// ★ 서버가 쓰는 부분. 렌더 정보가 전혀 없다.
	struct CollisionData
	{
		std::vector<Vec3>     vertices;
		std::vector<uint32_t> indices;        // 삼각형 목록
		Vec3 boundsMin{};
		Vec3 boundsMax{};

		bool Empty() const { return indices.empty(); }
	};

	// ★ 이름으로 고른 오브젝트의 삼각형 (2026-10-09) — ReadOptions::collectObject 가 채운다
	//   렌더 메시는 재질 기준으로 합쳐져 오브젝트 이름이 사라진다. 그래도 «이름으로 골라야 하는»
	//   기하(행성 접지면)가 필요해서, 리더가 한 번 훑는 동안 고른 오브젝트의 면을 여기에 따로 모은다.
	//   그래서 같은 파일을 렌더용·접지용으로 두 번 파싱하지 않아도 된다.
	//   위치와 삼각형만 있다. 와인딩은 렌더 메시와 같다. 지금은 OBJ 리더만 채운다.
	struct CollectedGeometry
	{
		std::vector<Vec3>        positions;
		std::vector<uint32_t>    indices;    // 삼각형 목록
		std::vector<std::string> objects;    // 면을 하나 이상 내놓은 오브젝트 이름 (중복 없음)

		bool Empty() const { return indices.empty(); }
	};

	struct ModelSource
	{
		std::vector<SourceMesh>     meshes;
		std::vector<SourceMaterial> materials;
		std::vector<SourceImage>    images;      // .glb 내장 이미지
		std::vector<SourceNode>     nodes;

		SkeletonSource               skeleton;   // 비어 있을 수 있다(정적 모델)
		std::vector<AnimationSource> animations; // 클립만 든 파일이면 meshes 가 비어 있다

		CollisionData collision;
		CollectedGeometry collected;   // ReadOptions::collectObject 가 있을 때만 채워진다

		Vec3 boundsMin{};
		Vec3 boundsMax{};

		ModelFormat format = ModelFormat::Unknown;

		// 텍스처 상대 경로를 푸는 기준.
		// ★ 와이드다 — 윈도우 경로는 원래 UTF-16 이다. narrow 로 두면 ACP(CP949) 왕복이 생겨
		//   한글 경로가 깨질 수 있다. 반대로 texturePaths 는 «파일에 적힌 바이트» 라 UTF-8 narrow 다.
		std::wstring sourceDirectory;
	};

	struct ReadOptions
	{
		// 서버용. 정점색·UV·탄젠트·재질·애니메이션을 읽지 않고 충돌과 위치만 채운다.
		bool geometryOnly = false;

		// 충돌 섹션이 없는 포맷(glTF·OBJ)이면 렌더 메시에서 만들어 넣는다.
		bool generateCollision = true;

		// 탄젠트가 파일에 없을 때 UV 기준으로 만든다. 노멀맵을 쓰면 필요하다.
		bool generateTangents = true;

		// 스킨·애니메이션을 읽는다. 서버는 끈다(Animator 는 클라 전용 — 명세 §14).
		bool readAnimation = true;

		// ★ OBJ 의 o/g 오브젝트마다 노드를 만들고 메시를 쪼갠다 (2026-10-08)
		//   끄면(기본) 재질 기준으로만 쪼갠다 — 메시 하나가 드로우 하나·BLAS 하나·TLAS 인스턴스
		//   하나라서, 켜면 드로우 콜이 폭증한다. 행성 OBJ 로 실측: 재질 기준 18개 vs 오브젝트×재질 3,708개
		//   (그래서 20fps 가 나왔다).
		//   이름으로 골라야 하는 기하(행성 접지면)는 이제 아래 collectObject 를 쓴다(2026-10-09).
		//   켜야 하는 경우는 «오브젝트별 노드» 자체가 필요한 기하 전용 경로뿐이다 —
		//   그 경로는 GPU 에 아무것도 올리지 않으므로 메시가 많아도 드로우가 늘지 않는다.
		bool splitByObject = false;

		// ★ 이름으로 오브젝트를 골라 그 면을 ModelSource::collected 에 따로 모은다 (2026-10-09)
		//   splitByObject 를 켜지 않아도 된다 — 렌더 메시는 재질 기준 그대로 두고,
		//   같은 한 번의 파싱에서 접지면만 추린다(Shared::PlanetSurface::IsGroundObject 를 넘긴다).
		//   비어 있으면 아무것도 모으지 않는다. 지금은 OBJ 리더만 지원한다(o/g 이름).
		std::function<bool(std::string_view)> collectObject;
	};

} // namespace Shared
