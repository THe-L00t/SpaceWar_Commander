#include "AssimpModelLoader.h"
#include "TextureLoader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <utility>

namespace swc {

	namespace {

		using namespace DirectX;
		namespace fs = std::filesystem;

		std::wstring FromUtf8(const char* value)
		{
			if (!value || !*value) return {};
			const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				value, -1, nullptr, 0);
			if (length <= 0) return {};
			std::wstring result(size_t(length), L'\0');
			if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				value, -1, result.data(), length)) return {};
			result.pop_back();
			return result;
		}

		bool Finite(const XMFLOAT3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool Invertible(const XMFLOAT4X4& value)
		{
			for (const auto& row : value.m)
				for (float entry : row)
					if (!std::isfinite(entry)) return false;
			const float determinant = XMVectorGetX(XMMatrixDeterminant(XMLoadFloat4x4(&value)));
			return std::isfinite(determinant) && std::fabs(determinant) > 1.0e-20f;
		}

		XMFLOAT3 ToVector(const aiVector3D& value)
		{
			return { float(value.x), float(value.y), float(value.z) };
		}

		// 열 벡터 행렬을 전치해 Scene의 행 벡터 규약(local * parentWorld)에 맞춘다.
		XMFLOAT4X4 ToMatrix(const aiMatrix4x4& value)
		{
			return {
				float(value.a1), float(value.b1), float(value.c1), float(value.d1),
				float(value.a2), float(value.b2), float(value.c2), float(value.d2),
				float(value.a3), float(value.b3), float(value.c3), float(value.d3),
				float(value.a4), float(value.b4), float(value.c4), float(value.d4) };
		}

		float Scalar(const aiMaterial& source, const char* key, unsigned int type,
			unsigned int index, float fallback)
		{
			ai_real value = fallback;
			if (source.Get(key, type, index, value) != AI_SUCCESS || !std::isfinite(value))
				return fallback;
			return float(value);
		}

		aiTextureType FindMap(const aiMaterial& source, std::initializer_list<aiTextureType> types)
		{
			for (aiTextureType type : types)
				if (source.GetTextureCount(type) != 0) return type;
			return aiTextureType_NONE;
		}

		bool IsRoughnessMap(const aiScene& scene, const aiMaterial& material)
		{
			aiString path;
			if (material.GetTexture(aiTextureType_SHININESS, 0, &path) != AI_SUCCESS) return false;
			const aiTexture* embedded = scene.GetEmbeddedTexture(path.C_Str());
			std::string name = embedded ? embedded->mFilename.C_Str() : path.C_Str();
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char ch) { return char(std::tolower(ch)); });
			// Meshy의 texture_0_roughness.png만 광택 슬롯에서 가져온다. 일반 광택 맵과 구분한다.
			const size_t slash = name.find_last_of("/\\");
			if (slash != std::string::npos) name.erase(0, slash + 1);
			return name.find("roughness") != std::string::npos;
		}

		fs::path FindTextureFile(const fs::path& modelFolder, const std::wstring& filename)
		{
			if (filename.empty()) return {};
			const fs::path referenced(filename);
			// 동봉한 상대 경로를 먼저 사용하고 제작 PC의 절대 경로는 마지막에 찾는다.
			const fs::path candidates[] = {
				referenced.is_relative() ? modelFolder / referenced : fs::path(),
				modelFolder / referenced.filename(),
				referenced.is_absolute() ? referenced : fs::path()
			};
			for (const fs::path& candidate : candidates)
			{
				std::error_code ec;
				if (!candidate.empty() && fs::is_regular_file(candidate, ec))
					return candidate.lexically_normal();
			}
			return {};
		}

		struct ImportContext
		{
			const aiScene& scene;
			ModelData& data;
			std::wstring& error;
			fs::path modelFolder;
			std::unordered_map<unsigned int, uint32_t> materialIndices;
			std::unordered_map<std::wstring, uint32_t> textureIndices;
			std::vector<unsigned int> materialUvChannels;
			bool hasBounds = false;

			bool LoadMap(const aiMaterial& source, aiTextureType type, TextureSlot slot,
				MaterialData& material, unsigned int& uvChannel)
			{
				if (type == aiTextureType_NONE) return true;
				aiString reference;
				aiTextureMapping mapping = aiTextureMapping_UV;
				unsigned int requestedUv = 0;
				if (source.GetTextureCount(type) != 1 ||
					source.GetTexture(type, 0, &reference, &mapping, &requestedUv) != AI_SUCCESS ||
					reference.length == 0)
				{
					error = L"겹친 텍스처 또는 유효하지 않은 재질 텍스처입니다: " + FromUtf8(material.name.c_str());
					return false;
				}
				aiUVTransform transform;
				source.Get(AI_MATKEY_UVTRANSFORM(type, 0), transform);
				if (mapping != aiTextureMapping_UV || requestedUv >= AI_MAX_NUMBER_OF_TEXTURECOORDS ||
					(uvChannel != UINT32_MAX && uvChannel != requestedUv) ||
					!std::isfinite(transform.mTranslation.x) || !std::isfinite(transform.mTranslation.y) ||
					!std::isfinite(transform.mScaling.x) || !std::isfinite(transform.mScaling.y) ||
					!std::isfinite(transform.mRotation) ||
					std::fabs(transform.mTranslation.x) > 1.0e-6f || std::fabs(transform.mTranslation.y) > 1.0e-6f ||
					std::fabs(transform.mScaling.x - 1.0f) > 1.0e-6f ||
					std::fabs(transform.mScaling.y - 1.0f) > 1.0e-6f || std::fabs(transform.mRotation) > 1.0e-6f)
				{
					error = L"한 재질은 변환 없는 동일한 UV 채널을 사용해야 합니다: " + FromUtf8(material.name.c_str());
					return false;
				}
				uvChannel = requestedUv;
				const bool srgb = slot == TextureSlot::BaseColor || slot == TextureSlot::Emissive;
				const bool normal = slot == TextureSlot::Normal;
				const aiTexture* embedded = scene.GetEmbeddedTexture(reference.C_Str());
				const std::wstring referencedName = FromUtf8(reference.C_Str());
				const fs::path external = embedded ? fs::path() : FindTextureFile(modelFolder, referencedName);
				if (!embedded && external.empty())
				{
					error = L"모델 참조 텍스처를 찾지 못했습니다: " + referencedName;
					return false;
				}
				const std::wstring sourceName = embedded ? L"embedded:" + referencedName : external.wstring();
				const std::wstring key = sourceName + (srgb ? L"|srgb" : normal ? L"|normal" : L"|linear");
				if (auto it = textureIndices.find(key); it != textureIndices.end())
				{
					material.textures[size_t(slot)] = it->second;
					return true;
				}
				TextureData image;
				if (embedded)
				{
					if (!embedded->pcData || embedded->mWidth == 0)
					{
						error = L"모델 내장 텍스처가 비어 있습니다: " + sourceName;
						return false;
					}
					if (embedded->mHeight == 0)
					{
						// mHeight가 0이면 mWidth는 PNG/JPEG 압축 바이트 수다. 파일 추출 없이 WIC에 전달한다.
						if (!LoadTextureImageFromMemory(embedded->pcData, size_t(embedded->mWidth),
							sourceName.c_str(), srgb, image, error)) return false;
					}
					else
					{
						const uint64_t byteCount = uint64_t(embedded->mWidth) * embedded->mHeight * 4;
						if (embedded->mWidth > 16384 || embedded->mHeight > 16384 || byteCount > UINT32_MAX)
						{
							error = L"내장 텍스처 크기가 지원 범위를 넘었습니다: " + sourceName;
							return false;
						}
						image.path = sourceName;
						image.width = embedded->mWidth;
						image.height = embedded->mHeight;
						image.srgb = srgb;
						image.pixels.resize(size_t(byteCount));
						for (size_t i = 0; i < size_t(byteCount / 4); ++i)
						{
							const aiTexel& texel = embedded->pcData[i];
							image.pixels[i * 4] = texel.r;
							image.pixels[i * 4 + 1] = texel.g;
							image.pixels[i * 4 + 2] = texel.b;
							image.pixels[i * 4 + 3] = texel.a;
						}
					}
				}
				else if (!LoadTextureImage(external.c_str(), srgb, image, error)) return false;
				if (normal)
				{
					// 현재 Meshy/Blender 원본은 +Y 노멀 맵이다. 반전된 V로 접선을 만들므로 G도 한 번 반전한다.
					for (size_t i = 1; i < image.pixels.size(); i += 4)
						image.pixels[i] = uint8_t(255 - image.pixels[i]);
				}
				if (data.textures.size() >= kInvalidModelIndex)
				{
					error = L"모델 텍스처 개수가 인덱스 범위를 넘었습니다.";
					return false;
				}
				const uint32_t index = uint32_t(data.textures.size());
				data.textures.push_back(std::move(image));
				textureIndices.emplace(key, index);
				material.textures[size_t(slot)] = index;
				return true;
			}

			bool LoadMaterial(unsigned int sourceIndex, uint32_t& index)
			{
				if (auto it = materialIndices.find(sourceIndex); it != materialIndices.end())
				{
					index = it->second;
					return true;
				}
				if (sourceIndex >= scene.mNumMaterials || !scene.mMaterials[sourceIndex])
				{
					error = L"모델 메시의 재질 인덱스가 유효하지 않습니다.";
					return false;
				}
				const aiMaterial& source = *scene.mMaterials[sourceIndex];
				MaterialData material;
				aiString name;
				source.Get(AI_MATKEY_NAME, name);
				material.name = name.C_Str();
				aiColor4D color(1, 1, 1, 1);
				if (source.Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
					source.Get(AI_MATKEY_COLOR_DIFFUSE, color);
				// Assimp의 FBX diffuse/emissive 색상에는 원본 Factor가 이미 곱해져 있다.
				material.baseColor = { float(color.r), float(color.g), float(color.b),
					std::clamp(float(color.a) * Scalar(source, AI_MATKEY_OPACITY, 1.0f), 0.0f, 1.0f) };
				aiColor3D emissive(0, 0, 0);
				source.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
				const float emission = Scalar(source, AI_MATKEY_EMISSIVE_INTENSITY, 1.0f);
				material.emissive = { float(emissive.r) * emission, float(emissive.g) * emission,
					float(emissive.b) * emission };
				const float shininess = std::max(0.0f, Scalar(source, AI_MATKEY_SHININESS, 2.7f));
				material.roughness = std::clamp(Scalar(source, AI_MATKEY_ROUGHNESS_FACTOR,
					std::sqrt(2.0f / (shininess + 2.0f))), 0.04f, 1.0f);
				material.metallic = std::clamp(Scalar(source, AI_MATKEY_METALLIC_FACTOR, 0.0f), 0.0f, 1.0f);
				if (!Finite({ material.baseColor.x, material.baseColor.y, material.baseColor.z }) ||
					!std::isfinite(material.baseColor.w) || !Finite(material.emissive))
				{
					error = L"모델 재질 색상에 유효하지 않은 값이 있습니다: " + FromUtf8(material.name.c_str());
					return false;
				}
				aiTextureType roughness = FindMap(source, { aiTextureType_DIFFUSE_ROUGHNESS });
				if (roughness == aiTextureType_NONE && IsRoughnessMap(scene, source))
					roughness = aiTextureType_SHININESS;
				unsigned int uvChannel = UINT32_MAX;
				if (!LoadMap(source, FindMap(source, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }),
					TextureSlot::BaseColor, material, uvChannel) ||
					!LoadMap(source, FindMap(source, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA }),
						TextureSlot::Normal, material, uvChannel) ||
					!LoadMap(source, roughness, TextureSlot::Roughness, material, uvChannel) ||
					!LoadMap(source, FindMap(source, { aiTextureType_METALNESS }),
						TextureSlot::Metallic, material, uvChannel) ||
					!LoadMap(source, FindMap(source, { aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE }),
						TextureSlot::Emissive, material, uvChannel)) return false;
				// Meshy의 두 PBR 맵은 완성된 값이다. Phong의 광택/반사 상수를 다시 곱하지 않는다.
				if (material.textures[size_t(TextureSlot::Roughness)] != kInvalidModelIndex) material.roughness = 1.0f;
				if (material.textures[size_t(TextureSlot::Metallic)] != kInvalidModelIndex) material.metallic = 1.0f;
				if (data.materials.size() >= kInvalidModelIndex)
				{
					error = L"모델 재질 개수가 인덱스 범위를 넘었습니다.";
					return false;
				}
				index = uint32_t(data.materials.size());
				data.materials.push_back(std::move(material));
				materialUvChannels.push_back(uvChannel == UINT32_MAX ? 0 : uvChannel);
				materialIndices.emplace(sourceIndex, index);
				return true;
			}

			void IncludePoint(const XMFLOAT3& point)
			{
				if (!hasBounds)
				{
					data.boundsMin = data.boundsMax = point;
					hasBounds = true;
					return;
				}
				data.boundsMin.x = std::min(data.boundsMin.x, point.x);
				data.boundsMin.y = std::min(data.boundsMin.y, point.y);
				data.boundsMin.z = std::min(data.boundsMin.z, point.z);
				data.boundsMax.x = std::max(data.boundsMax.x, point.x);
				data.boundsMax.y = std::max(data.boundsMax.y, point.y);
				data.boundsMax.z = std::max(data.boundsMax.z, point.z);
			}
		};

		void MakeTangents(Vertex (&vertices)[3])
		{
			const XMVECTOR edge1 = XMLoadFloat3(&vertices[1].position) - XMLoadFloat3(&vertices[0].position);
			const XMVECTOR edge2 = XMLoadFloat3(&vertices[2].position) - XMLoadFloat3(&vertices[0].position);
			const float u1 = vertices[1].uv.x - vertices[0].uv.x;
			const float v1 = vertices[1].uv.y - vertices[0].uv.y;
			const float u2 = vertices[2].uv.x - vertices[0].uv.x;
			const float v2 = vertices[2].uv.y - vertices[0].uv.y;
			const float determinant = u1 * v2 - u2 * v1;
			const bool validUv = std::isfinite(determinant) && std::fabs(determinant) > 1.0e-12f;
			const XMVECTOR tangent = validUv ? (edge1 * v2 - edge2 * v1) / determinant : edge1;
			const XMVECTOR bitangent = validUv ? (edge2 * u1 - edge1 * u2) / determinant : edge2;
			for (Vertex& vertex : vertices)
			{
				const XMVECTOR normal = XMLoadFloat3(&vertex.normal);
				XMVECTOR t = tangent - normal * XMVector3Dot(normal, tangent);
				if (XMVectorGetX(XMVector3LengthSq(t)) < 1.0e-12f)
				{
					const XMVECTOR axis = std::fabs(vertex.normal.y) < 0.9f
						? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
					t = XMVector3Cross(axis, normal);
				}
				t = XMVector3Normalize(t);
				const float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(normal, t), bitangent)) < 0 ? -1.0f : 1.0f;
				XMStoreFloat4(&vertex.tangent, XMVectorSetW(t, sign));
			}
		}

		bool LoadMesh(const aiMesh& source, uint32_t nodeIndex, const XMFLOAT4X4& global,
			ImportContext& context)
		{
			if (!source.HasPositions() || !source.HasNormals() || !source.HasFaces())
			{
				context.error = L"모델 메시의 정점, 법선 또는 면 데이터가 없습니다: " + FromUtf8(source.mName.C_Str());
				return false;
			}
			ModelMeshData target;
			if (!context.LoadMaterial(source.mMaterialIndex, target.material)) return false;
			const unsigned int uvChannel = context.materialUvChannels[target.material];
			const MaterialData& material = context.data.materials[target.material];
			const bool textured = std::any_of(material.textures.begin(), material.textures.end(),
				[](uint32_t texture) { return texture != kInvalidModelIndex; });
			const bool hasUv = source.HasTextureCoords(uvChannel);
			if (textured && (!hasUv || source.mNumUVComponents[uvChannel] < 2))
			{
				context.error = L"텍스처가 있는 메시의 UV 채널이 없습니다: " + FromUtf8(source.mName.C_Str());
				return false;
			}
			if (source.mNumFaces > UINT32_MAX / 3)
			{
				context.error = L"모델 메시가 32비트 정점 인덱스 범위를 넘었습니다.";
				return false;
			}
			target.mesh.vertices.reserve(size_t(source.mNumFaces) * 3);
			target.mesh.indices.reserve(size_t(source.mNumFaces) * 3);
			const XMMATRIX world = XMLoadFloat4x4(&global);
			const float worldSign = XMVectorGetX(XMMatrixDeterminant(world)) < 0 ? -1.0f : 1.0f;
			for (unsigned int faceIndex = 0; faceIndex < source.mNumFaces; ++faceIndex)
			{
				const aiFace& face = source.mFaces[faceIndex];
				if (face.mNumIndices < 3) continue;
				if (face.mNumIndices != 3 || !face.mIndices)
				{
					context.error = L"삼각형 변환 후에도 삼각형이 아닌 모델 면이 남았습니다.";
					return false;
				}
				Vertex vertices[3];
				for (unsigned int corner = 0; corner < 3; ++corner)
				{
					const unsigned int vertexIndex = face.mIndices[corner];
					if (vertexIndex >= source.mNumVertices)
					{
						context.error = L"모델 정점 인덱스가 범위를 벗어났습니다.";
						return false;
					}
					Vertex& vertex = vertices[corner];
					vertex.position = ToVector(source.mVertices[vertexIndex]);
					vertex.normal = ToVector(source.mNormals[vertexIndex]);
					if (source.HasVertexColors(0))
					{
						const aiColor4D& color = source.mColors[0][vertexIndex];
						vertex.color = { float(color.r), float(color.g), float(color.b) };
					}
					if (hasUv)
					{
						const aiVector3D& uv = source.mTextureCoords[uvChannel][vertexIndex];
						vertex.uv = { float(uv.x), float(uv.y) }; // V는 Assimp에서 이미 반전했다.
					}
					const float normalLengthSq = XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&vertex.normal)));
					if (!Finite(vertex.position) || !Finite(vertex.normal) || !Finite(vertex.color) ||
						!std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y) ||
						!std::isfinite(normalLengthSq) || normalLengthSq < 1.0e-20f)
					{
						context.error = L"모델 정점 데이터가 유효하지 않습니다.";
						return false;
					}
					XMStoreFloat3(&vertex.normal, XMVector3Normalize(XMLoadFloat3(&vertex.normal)));
				}
				const XMVECTOR edge1 = XMLoadFloat3(&vertices[1].position) - XMLoadFloat3(&vertices[0].position);
				const XMVECTOR edge2 = XMLoadFloat3(&vertices[2].position) - XMLoadFloat3(&vertices[0].position);
				const XMVECTOR faceNormal = XMVector3Cross(edge1, edge2);
				if (XMVectorGetX(XMVector3LengthSq(faceNormal)) < 1.0e-20f) continue;
				const XMVECTOR averageNormal = XMLoadFloat3(&vertices[0].normal) +
					XMLoadFloat3(&vertices[1].normal) + XMLoadFloat3(&vertices[2].normal);
				// 계층의 음수 스케일에도 CW 전면과 변환된 법선이 일치하도록 한다.
				if (worldSign * XMVectorGetX(XMVector3Dot(faceNormal, averageNormal)) < 0)
					std::swap(vertices[1], vertices[2]);
				MakeTangents(vertices);
				for (const Vertex& vertex : vertices)
				{
					if (!Finite({ vertex.tangent.x, vertex.tangent.y, vertex.tangent.z }))
					{
						context.error = L"모델 접선 계산 중 유효하지 않은 값이 나왔습니다.";
						return false;
					}
					target.mesh.indices.push_back(uint32_t(target.mesh.vertices.size()));
					target.mesh.vertices.push_back(vertex);
					XMFLOAT3 modelPosition;
					XMStoreFloat3(&modelPosition, XMVector3TransformCoord(XMLoadFloat3(&vertex.position), world));
					if (!Finite(modelPosition))
					{
						context.error = L"모델 경계 계산 중 유효하지 않은 좌표가 나왔습니다.";
						return false;
					}
					context.IncludePoint(modelPosition);
				}
			}
			if (target.mesh.indices.empty()) return true;
			if (context.data.meshes.size() >= kInvalidModelIndex)
			{
				context.error = L"모델 메시 개수가 인덱스 범위를 넘었습니다.";
				return false;
			}
			const uint32_t meshIndex = uint32_t(context.data.meshes.size());
			context.data.meshes.push_back(std::move(target));
			context.data.nodes[nodeIndex].meshes.push_back(meshIndex);
			return true;
		}

		bool LoadNode(const aiNode& source, uint32_t parent, const XMFLOAT4X4& parentGlobal,
			unsigned int depth, ImportContext& context)
		{
			if (depth > 512 || context.data.nodes.size() >= kInvalidModelIndex ||
				(source.mNumMeshes > 0 && !source.mMeshes) || (source.mNumChildren > 0 && !source.mChildren))
			{
				context.error = L"모델 노드 계층이 지원 범위를 벗어났습니다.";
				return false;
			}
			ModelNodeData node;
			node.name = source.mName.C_Str();
			node.parent = parent;
			node.local = ToMatrix(source.mTransformation);
			if (parent == kInvalidModelIndex)
			{
				// 기존 모델은 +Y 위, +Z 정면, -X 좌표축의 DirectX 축 변환으로 표시했다.
				// Assimp의 Z 반사를 그 기준의 X 반사로 맞춰 기존 정면 방향을 유지한다.
				XMStoreFloat4x4(&node.local, XMLoadFloat4x4(&node.local) * XMMatrixRotationY(XM_PI));
			}
			XMFLOAT4X4 global;
			XMStoreFloat4x4(&global, XMLoadFloat4x4(&node.local) * XMLoadFloat4x4(&parentGlobal));
			if (!Invertible(node.local) || !Invertible(global))
			{
				context.error = L"모델 노드 변환이 유효하지 않습니다: " + FromUtf8(source.mName.C_Str());
				return false;
			}
			const uint32_t nodeIndex = uint32_t(context.data.nodes.size());
			context.data.nodes.push_back(std::move(node));
			for (unsigned int i = 0; i < source.mNumMeshes; ++i)
			{
				const unsigned int meshIndex = source.mMeshes[i];
				if (meshIndex >= context.scene.mNumMeshes || !context.scene.mMeshes[meshIndex])
				{
					context.error = L"모델 노드의 메시 인덱스가 유효하지 않습니다.";
					return false;
				}
				if (!LoadMesh(*context.scene.mMeshes[meshIndex], nodeIndex, global, context)) return false;
			}
			for (unsigned int i = 0; i < source.mNumChildren; ++i)
			{
				if (!source.mChildren[i])
				{
					context.error = L"모델 자식 노드가 비어 있습니다.";
					return false;
				}
				if (!LoadNode(*source.mChildren[i], nodeIndex, global, depth + 1, context)) return false;
			}
			return true;
		}
	}

	bool AssimpModelLoader::Load(const wchar_t* path, ModelData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path)
		{
			error = L"모델 파일 경로가 비어 있습니다.";
			return false;
		}
		std::error_code ec;
		const fs::path modelPath = fs::absolute(fs::path(path), ec).lexically_normal();
		if (ec || !fs::is_regular_file(modelPath, ec))
		{
			error = L"모델 파일을 찾지 못했습니다: " + std::wstring(path);
			return false;
		}
		const uintmax_t byteCount = fs::file_size(modelPath, ec);
		if (ec || byteCount == 0 || byteCount > uintmax_t(std::numeric_limits<std::streamsize>::max()) ||
			byteCount > uintmax_t(std::numeric_limits<size_t>::max()))
		{
			error = L"모델 파일 크기를 읽을 수 없거나 지원 범위를 넘었습니다: " + modelPath.wstring();
			return false;
		}
		// 한글 Windows 경로는 filesystem::path로 연다. FBX는 단일 파일이므로 내용만 전달해도 된다.
		// 외부 이미지는 위에서 모델 폴더 기준으로 별도 해석한다.
		std::ifstream file(modelPath, std::ios::binary);
		std::vector<char> bytes(static_cast<size_t>(byteCount));
		if (!file || !file.read(bytes.data(), static_cast<std::streamsize>(byteCount)))
		{
			error = L"모델 파일을 읽지 못했습니다: " + modelPath.wstring();
			return false;
		}
		Assimp::Importer importer;
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, false);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, false);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, false);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, false);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, true);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, false);
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_LEGACY_NAMING, true);
		// 피벗과 계층을 보존한다. PreTransformVertices/OptimizeGraph는 적용하지 않는다.
		// 좌수계, V, 면 순서는 이 단계에서 한 번만 변환한다. 접선은 최종 UV로 계산한다.
		const unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
			aiProcess_MakeLeftHanded | aiProcess_FlipUVs | aiProcess_FlipWindingOrder |
			aiProcess_ValidateDataStructure;
		const aiScene* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), flags, "fbx");
		if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || !scene->mRootNode ||
			!scene->mMeshes || scene->mNumMeshes == 0 || !scene->mMaterials)
		{
			error = L"Assimp 모델 읽기 실패: " + FromUtf8(importer.GetErrorString()) + L"\n" + modelPath.wstring();
			return false;
		}
		ModelData data;
		ImportContext context{ *scene, data, error, modelPath.parent_path() };
		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());
		if (!LoadNode(*scene->mRootNode, kInvalidModelIndex, identity, 0, context)) return false;
		if (data.meshes.empty() || !context.hasBounds)
		{
			error = L"모델 파일에 표시할 삼각형 메시가 없습니다.";
			return false;
		}
		out = std::move(data);
		return true;
	}
}
