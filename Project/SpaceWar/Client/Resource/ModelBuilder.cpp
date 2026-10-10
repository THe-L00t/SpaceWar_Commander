#include "ModelBuilder.h"
#include "TextureLoader.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

using namespace DirectX;

namespace swc {

	namespace {

		namespace fs = std::filesystem;

		// 파일에 적힌 텍스처 경로는 UTF-8 바이트다. ACP 를 거치지 않고 와이드로 올린다.
		fs::path Utf8Path(const std::string& text)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		}

		// 슬롯 순서는 Shared 쪽 SourceTextureSlot 과 ModelData.h 의 TextureSlot 이 같다.
		// 셰이더의 t1~t5 까지 같은 순서다 — 한 곳이라도 어긋나면 엉뚱한 맵이 들어간다.
		static_assert(Shared::kSourceTextureCount == kMaterialTextureCount,
			"텍스처 슬롯 수가 Shared 와 Client 에서 다르다");
		static_assert(static_cast<size_t>(Shared::SourceTextureSlot::BaseColor) ==
			static_cast<size_t>(TextureSlot::BaseColor) &&
			static_cast<size_t>(Shared::SourceTextureSlot::Normal) ==
			static_cast<size_t>(TextureSlot::Normal) &&
			static_cast<size_t>(Shared::SourceTextureSlot::Roughness) ==
			static_cast<size_t>(TextureSlot::Roughness) &&
			static_cast<size_t>(Shared::SourceTextureSlot::Metallic) ==
			static_cast<size_t>(TextureSlot::Metallic) &&
			static_cast<size_t>(Shared::SourceTextureSlot::Emissive) ==
			static_cast<size_t>(TextureSlot::Emissive),
			"텍스처 슬롯 순서가 Shared 와 Client 에서 다르다");

		static_assert(Shared::kJointsPerVertex == 4,
			"SkinVertex 가 조인트 4개 기준으로 선언돼 있다");

		XMFLOAT4X4 ToMatrix(const Shared::Mat4& source)
		{
			// 둘 다 행 우선 4x4 다. 16개를 그대로 옮긴다.
			XMFLOAT4X4 matrix;
			for (int row = 0; row < 4; ++row)
			{
				for (int column = 0; column < 4; ++column)
					matrix.m[row][column] = source.m[row * 4 + column];
			}
			return matrix;
		}

		// 텍스처는 «출처 + 용도» 로 묶어 한 번만 읽는다.
		class TextureCache
		{
		public:
			TextureCache(ModelData& data, const Shared::ModelSource& source)
				: data(data), source(source) {}

			bool ResolveFile(const std::string& relative, TextureSlot slot,
				uint32_t& index, std::wstring& error)
			{
				index = kInvalidModelIndex;
				if (relative.empty()) return true;

				fs::path path = Utf8Path(relative);
				if (path.is_relative() && !source.sourceDirectory.empty())
					path = fs::path(source.sourceDirectory) / path;
				path = path.lexically_normal();

				const bool srgb = IsColorSlot(slot);
				const std::wstring key = path.wstring() + (srgb ? L"|srgb" : L"|linear");
				if (auto it = loaded.find(key); it != loaded.end())
				{
					index = it->second;
					return true;
				}

				TextureData image;
				if (!LoadTextureImage(path.c_str(), srgb, image, error))
				{
					error = L"모델 텍스처를 읽지 못했습니다: " + path.wstring() + L"\n" + error;
					return false;
				}
				index = Store(std::move(image), key);
				return true;
			}

			// .glb 안에 들어 있던 이미지. 파일로 꺼내지 않고 메모리에서 바로 디코딩한다
			// (FBX SDK 시절의 «임시폴더 추출» 단계가 없어진 자리다).
			bool ResolveEmbedded(uint32_t imageIndex, TextureSlot slot,
				uint32_t& index, std::wstring& error)
			{
				index = kInvalidModelIndex;
				if (imageIndex == Shared::kInvalidIndex || imageIndex >= source.images.size())
					return true;

				const Shared::SourceImage& image = source.images[imageIndex];
				const bool srgb = IsColorSlot(slot);
				const std::wstring key = L"embedded:" + std::to_wstring(imageIndex) +
					(srgb ? L"|srgb" : L"|linear");
				if (auto it = loaded.find(key); it != loaded.end())
				{
					index = it->second;
					return true;
				}

				const std::wstring label = L"내장 이미지 #" + std::to_wstring(imageIndex);
				TextureData decoded;
				if (!LoadTextureImageFromMemory(image.bytes.data(), image.bytes.size(),
					srgb, label.c_str(), decoded, error))
					return false;

				index = Store(std::move(decoded), key);
				return true;
			}

		private:
			// sRGB 로 읽을 슬롯 = 색을 담은 슬롯. 거칠기·금속도·노멀은 데이터라 선형이다.
			static bool IsColorSlot(TextureSlot slot)
			{
				return slot == TextureSlot::BaseColor || slot == TextureSlot::Emissive;
			}

			uint32_t Store(TextureData&& image, const std::wstring& key)
			{
				const uint32_t index = static_cast<uint32_t>(data.textures.size());
				data.textures.push_back(std::move(image));
				loaded.emplace(key, index);
				return index;
			}

			ModelData& data;
			const Shared::ModelSource& source;
			std::unordered_map<std::wstring, uint32_t> loaded;
		};

	} // namespace

	bool BuildModelData(Shared::ModelSource& source, ModelData& out, std::wstring& error)
	{
		error.clear();
		out = ModelData{};

		if (source.meshes.empty() || source.nodes.empty())
		{
			error = L"모델에 메시 또는 노드가 없습니다.";
			return false;
		}

		// ── 재질·텍스처 ────────────────────────────────────
		TextureCache textures(out, source);
		out.materials.reserve(source.materials.size());
		for (const Shared::SourceMaterial& sourceMaterial : source.materials)
		{
			MaterialData material;
			material.name = sourceMaterial.name;
			material.baseColor = { sourceMaterial.baseColor.x, sourceMaterial.baseColor.y,
				sourceMaterial.baseColor.z, sourceMaterial.baseColor.w };
			material.emissive = { sourceMaterial.emissive.x, sourceMaterial.emissive.y,
				sourceMaterial.emissive.z };
			material.roughness = sourceMaterial.roughness;
			material.metallic = sourceMaterial.metallic;

			for (size_t slot = 0; slot < kMaterialTextureCount; ++slot)
			{
				const TextureSlot which = static_cast<TextureSlot>(slot);
				// 외부 파일이 먼저다. 없으면 내장 이미지를 본다.
				if (!textures.ResolveFile(sourceMaterial.texturePaths[slot], which,
					material.textures[slot], error))
					return false;
				if (material.textures[slot] == kInvalidModelIndex &&
					!textures.ResolveEmbedded(sourceMaterial.embeddedImages[slot], which,
						material.textures[slot], error))
					return false;
			}

			// 맵이 있으면 스칼라는 1 로 두고 셰이더가 맵을 그대로 쓰게 한다.
			if (material.textures[size_t(TextureSlot::Roughness)] != kInvalidModelIndex)
				material.roughness = 1.0f;
			if (material.textures[size_t(TextureSlot::Metallic)] != kInvalidModelIndex)
				material.metallic = 1.0f;

			out.materials.push_back(std::move(material));
		}

		// ── 메시 ───────────────────────────────────────────
		// ★ 정점·인덱스·스킨은 복사하지 않고 «넘겨받는다» (2026-10-09)
		//   파서가 이미 렌더 배치(Vertex 60B)로 만들었다. 배열을 통째로 옮기므로 이 순간에도
		//   정점은 한 벌뿐이다. 넘겨준 쪽(source.meshes)은 빈 배열로 남는다.
		out.meshes.reserve(source.meshes.size());
		for (Shared::SourceMesh& sourceMesh : source.meshes)
		{
			ModelMeshData mesh;
			mesh.material = sourceMesh.material == Shared::kInvalidIndex
				? kInvalidModelIndex : sourceMesh.material;
			if (mesh.material != kInvalidModelIndex && mesh.material >= out.materials.size())
			{
				error = L"모델 메시의 재질 참조가 범위를 벗어났습니다.";
				return false;
			}

			mesh.mesh.vertices = std::move(sourceMesh.vertices);
			mesh.mesh.indices = std::move(sourceMesh.indices);
			mesh.skin = std::move(sourceMesh.skin);   // 스킨 메시가 아니면 빈 배열

			out.meshes.push_back(std::move(mesh));
		}

		// ── 노드 계층 ──────────────────────────────────────
		//  둘 다 «부모가 먼저» 규칙이라 순서를 그대로 옮긴다(Shared 쪽에서 이미 검사했다).
		out.nodes.reserve(source.nodes.size());
		for (const Shared::SourceNode& sourceNode : source.nodes)
		{
			ModelNodeData node;
			node.name = sourceNode.name;
			node.parent = sourceNode.parent == Shared::kInvalidIndex
				? kInvalidModelIndex : sourceNode.parent;
			node.meshes = sourceNode.meshes;
			node.local = ToMatrix(sourceNode.local);
			out.nodes.push_back(std::move(node));
		}

		out.boundsMin = { source.boundsMin.x, source.boundsMin.y, source.boundsMin.z };
		out.boundsMax = { source.boundsMax.x, source.boundsMax.y, source.boundsMax.z };
		return true;
	}

	void BuildSkeleton(const Shared::ModelSource& source, SkeletonResource& out)
	{
		out = SkeletonResource{};
		if (source.skeleton.Empty()) return;

		out.name = source.skeleton.name;
		out.joints.reserve(source.skeleton.joints.size());
		for (const Shared::SourceJoint& joint : source.skeleton.joints)
		{
			JointData data;
			data.name = joint.name;
			data.parent = joint.parent == Shared::kInvalidIndex ? kInvalidJoint : joint.parent;
			data.localRest = ToMatrix(joint.localRest);
			data.inverseBind = ToMatrix(joint.inverseBind);

			// ★ 바인드 자세를 지금 한 번만 TRS 로 푼다
			//   포즈 블렌딩이 TRS 공간에서 돌아가므로(AnimationBlender), 매 프레임 행렬을
			//   분해하지 않도록 로드 시점에 풀어 둔다. 분해가 실패하는 행렬(전단·0 배율)이면
			//   기본값(이동 0 · 회전 없음 · 배율 1)을 남긴다.
			XMVECTOR scale, rotation, translation;
			if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&data.localRest)))
			{
				XMStoreFloat3(&data.restTranslation, translation);
				XMStoreFloat4(&data.restRotation, XMQuaternionNormalize(rotation));
				XMStoreFloat3(&data.restScale, scale);
			}

			out.joints.push_back(std::move(data));
		}
	}

	void BuildAnimationClips(const Shared::ModelSource& source, std::vector<AnimationClipData>& out)
	{
		out.clear();
		out.reserve(source.animations.size());

		for (const Shared::AnimationSource& sourceClip : source.animations)
		{
			AnimationClipData clip;
			clip.name = sourceClip.name;
			clip.duration = sourceClip.duration;
			clip.channels.reserve(sourceClip.channels.size());

			for (const Shared::AnimationChannel& sourceChannel : sourceClip.channels)
			{
				AnimationChannelData channel;
				channel.joint = sourceChannel.joint == Shared::kInvalidIndex
					? kInvalidJoint : sourceChannel.joint;
				switch (sourceChannel.path)
				{
				case Shared::AnimationPath::Rotation: channel.path = AnimationPath::Rotation; break;
				case Shared::AnimationPath::Scale:    channel.path = AnimationPath::Scale; break;
				default:                              channel.path = AnimationPath::Translation; break;
				}
				channel.interpolation = sourceChannel.interpolation == Shared::AnimationInterpolation::Step
					? AnimationInterpolation::Step : AnimationInterpolation::Linear;
				channel.times = sourceChannel.times;
				channel.values = sourceChannel.values;
				clip.channels.push_back(std::move(channel));
			}

			out.push_back(std::move(clip));
		}
	}
}
