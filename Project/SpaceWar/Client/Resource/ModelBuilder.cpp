#include "ModelBuilder.h"
#include "TextureLoader.h"

#include <cstring>
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
				fs::path ddsPath = path;
				ddsPath.replace_extension(L".dds");
				if (TryMipmappedDds(ddsPath, srgb, 0, 0, image))
				{
					++stats.dds;
				}
				else
				{
					if (!LoadTextureImage(path.c_str(), srgb, image, error))
					{
						error = L"모델 텍스처를 읽지 못했습니다: " + path.wstring() + L"\n" + error;
						return false;
					}
					++stats.decoded;
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

				TextureData decoded;
				uint32_t pngWidth = 0, pngHeight = 0;
				PngSize(image.bytes, pngWidth, pngHeight);
				const fs::path ddsPath = EmbeddedDdsPath(image.name);
				if (!ddsPath.empty() && TryMipmappedDds(ddsPath, srgb, pngWidth, pngHeight, decoded))
				{
					++stats.dds;
				}
				else
				{
					const std::wstring label = L"내장 이미지 #" + std::to_wstring(imageIndex);
					if (!LoadTextureImageFromMemory(image.bytes.data(), image.bytes.size(),
						srgb, label.c_str(), decoded, error))
						return false;
					++stats.decoded;
				}

				index = Store(std::move(decoded), key);
				return true;
			}

			TextureLoadStats stats;

		private:
			// ★ 텍스처 LOD = 밉맵 (2026-10-29 사용자 «일단 텍스처만 LOD 시작해줘» → «미리 만든 DDS 를 읽는다»)
			//   노션 «LOD 적용 준비» 2.4 «밉맵이 텍스처 LOD 다». resource(dfa4fe4)가 PNG 옆에 같은 이름으로 구워 둔
			//   밉맵 DDS(texconv, 1×1 까지)가 있으면 그것을 읽는다. 렌더러는 밉 체인을 그대로 올린다(GRenderer::CreateTexture).
			//   없거나 맞지 않으면(RGBA8 아님 · sRGB 가 슬롯과 다름 · 크기가 PNG 와 다름) 예전처럼 PNG 를 WIC 로 푼다(밉 1단).
			//   방식은 client1 세션이 10-28 에 넣었다 되돌린 코드와 같다(되돌린 이유 = 먼 곳 번쩍임 → 렌더러에서 원인을 찾는다).
			static bool TryMipmappedDds(const fs::path& ddsPath, bool srgb, uint32_t expectWidth, uint32_t expectHeight,
				TextureData& out)
			{
				std::error_code ec;
				if (!fs::is_regular_file(ddsPath, ec)) return false;
				TextureData dds;
				std::wstring ddsError;
				if (!LoadDdsTexture(ddsPath.c_str(), dds, ddsError)) return false;
				if (dds.format != TextureFormat::Rgba8 || dds.srgb != srgb) return false;
				if (expectWidth != 0 && (dds.width != expectWidth || dds.height != expectHeight)) return false;
				out = std::move(dds);
				return true;
			}

			// glb 내장 이미지의 DDS 자리 = 모델 파일 폴더\<이미지 이름>.dds (리소스 규칙: 이미지 이름 = PNG 파일 이름).
			// 이름이 없거나 폴더가 섞여 있으면 찾지 않는다.
			fs::path EmbeddedDdsPath(const std::string& imageName) const
			{
				if (imageName.empty() || source.sourceDirectory.empty()) return {};
				const fs::path name = Utf8Path(imageName);
				if (name.has_parent_path() || !name.has_filename()) return {};
				return fs::path(source.sourceDirectory) / (name.wstring() + L".dds");
			}

			// PNG 머리(IHDR)의 크기. PNG 가 아니면 0 × 0 — 크기 비교를 하지 않는다.
			static void PngSize(const std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height)
			{
				width = height = 0;
				static constexpr uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
				if (bytes.size() < 24 || std::memcmp(bytes.data(), kSignature, 8) != 0 ||
					std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) return;
				auto be32 = [&](size_t at)
				{
					return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) |
						(uint32_t(bytes[at + 2]) << 8) | uint32_t(bytes[at + 3]);
				};
				width = be32(16);
				height = be32(20);
			}

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

	bool BuildModelData(Shared::ModelSource& source, ModelData& out, std::wstring& error,
		TextureLoadStats* stats)
	{
		error.clear();
		out = ModelData{};
		if (stats) *stats = TextureLoadStats{};

		if (source.meshes.empty() || source.nodes.empty())
		{
			error = L"모델에 메시 또는 노드가 없습니다.";
			return false;
		}

		// ── 재질·텍스처 ────────────────────────────────────
		TextureCache textures(out, source);
		out.materials.reserve(source.materials.size());

		// 화면 메시가 쓰지 않는 재질(숨긴 충돌 프록시 등)의 텍스처는 디코딩하지 않는다 (client2 2f15728).
		// 재질 번호는 그대로 둬야 하므로 재질 자체는 남기고 텍스처만 건너뛴다.
		std::vector<bool> materialUsed(source.materials.size(), false);
		for (const Shared::SourceMesh& sourceMesh : source.meshes)
			if (sourceMesh.material < materialUsed.size()) materialUsed[sourceMesh.material] = true;

		for (size_t materialIndex = 0; materialIndex < source.materials.size(); ++materialIndex)
		{
			const Shared::SourceMaterial& sourceMaterial = source.materials[materialIndex];
			MaterialData material;
			material.name = sourceMaterial.name;
			material.baseColor = { sourceMaterial.baseColor.x, sourceMaterial.baseColor.y,
				sourceMaterial.baseColor.z, sourceMaterial.baseColor.w };
			material.emissive = { sourceMaterial.emissive.x, sourceMaterial.emissive.y,
				sourceMaterial.emissive.z };
			material.roughness = sourceMaterial.roughness;
			material.metallic = sourceMaterial.metallic;
			material.doubleSided = sourceMaterial.doubleSided;
			// 투과 · 부피 · 굴절률 (2026-10-28) — 이름 · 뜻 · 기본값이 같아 그대로 옮긴다(ModelSource.h).
			material.transmission = sourceMaterial.transmission;
			material.thickness = sourceMaterial.thickness;
			material.attenuationDistance = sourceMaterial.attenuationDistance;
			material.attenuationColor = { sourceMaterial.attenuationColor.x, sourceMaterial.attenuationColor.y,
				sourceMaterial.attenuationColor.z };
			material.ior = sourceMaterial.ior;
			// 알파 마스크 (2026-10-28) — 이름 · 뜻 · 기본값이 같아 그대로 옮긴다
			material.alphaMask = sourceMaterial.alphaMask;
			material.alphaCutoff = sourceMaterial.alphaCutoff;

			for (size_t slot = 0; materialUsed[materialIndex] && slot < kMaterialTextureCount; ++slot)
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
		if (stats) *stats = textures.stats;

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
