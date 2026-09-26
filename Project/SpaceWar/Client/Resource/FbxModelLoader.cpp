#include "FbxModelLoader.h"
#include "TextureLoader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fbxsdk.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace swc {

	namespace {

		using namespace DirectX;
		namespace fs = std::filesystem;

		struct FbxDestroy
		{
			template<class T> void operator()(T* object) const
			{
				if (object) object->Destroy();
			}
		};

		std::string ToUtf8(const std::wstring& value)
		{
			if (value.empty()) return {};
			const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
				value.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (length <= 0) return {};
			std::string result(size_t(length), '\0');
			if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
				result.data(), length, nullptr, nullptr)) return {};
			result.pop_back();
			return result;
		}

		std::wstring FromUtf8(const char* value)
		{
			if (!value || !*value) return {};
			const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				value, -1, nullptr, 0);
			if (length <= 0) return L"(UTF-8 변환 실패)";
			std::wstring result(size_t(length), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
				result.data(), length);
			result.pop_back();
			return result;
		}

		// SDK의 메모리 배치에 의존하지 않고 기저와 원점을 변환한다.
		// 결과는 DirectX의 행 벡터 규약: local * parentWorld.
		XMFLOAT4X4 ToMatrix(const FbxAMatrix& matrix)
		{
			const FbxVector4 origin = matrix.MultT(FbxVector4(0, 0, 0));
			const FbxVector4 x = matrix.MultT(FbxVector4(1, 0, 0)) - origin;
			const FbxVector4 y = matrix.MultT(FbxVector4(0, 1, 0)) - origin;
			const FbxVector4 z = matrix.MultT(FbxVector4(0, 0, 1)) - origin;
			return {
				float(x[0]), float(x[1]), float(x[2]), 0.0f,
				float(y[0]), float(y[1]), float(y[2]), 0.0f,
				float(z[0]), float(z[1]), float(z[2]), 0.0f,
				float(origin[0]), float(origin[1]), float(origin[2]), 1.0f };
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

		FbxProperty FindProperty(FbxSurfaceMaterial* material,
			std::initializer_list<const char*> names, bool connectedOnly = false)
		{
			for (const char* name : names)
			{
				FbxProperty property = material->FindProperty(name);
				if (property.IsValid() && (!connectedOnly || property.GetSrcObjectCount<FbxTexture>() > 0))
					return property;
			}
			return {};
		}

		float Scalar(FbxSurfaceMaterial* material,
			std::initializer_list<const char*> names, float fallback)
		{
			const FbxProperty property = FindProperty(material, names);
			if (!property.IsValid()) return fallback;
			const float value = float(property.Get<FbxDouble>());
			return std::isfinite(value) ? value : fallback;
		}

		XMFLOAT3 Color(FbxSurfaceMaterial* material,
			std::initializer_list<const char*> names, XMFLOAT3 fallback)
		{
			const FbxProperty property = FindProperty(material, names);
			if (!property.IsValid()) return fallback;
			const FbxDouble3 color = property.Get<FbxDouble3>();
			const XMFLOAT3 result{ float(color[0]), float(color[1]), float(color[2]) };
			return Finite(result) ? result : fallback;
		}

		// Shininess/Reflection 슬롯은 일반적으로 PBR과 의미가 다르다.
		// 이번 Meshy/Blender FBX의 연결 속성과 텍스처 객체 이름을 함께 확인한다.
		FbxProperty ExportedPbrMap(FbxSurfaceMaterial* material, const char* propertyName,
			const char* textureName)
		{
			FbxProperty property = material->FindProperty(propertyName);
			if (!property.IsValid() || property.GetSrcObjectCount<FbxFileTexture>() != 1) return {};
			FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>();
			return texture && std::string(texture->GetName()) == textureName ? property : FbxProperty();
		}

		fs::path FindTextureFile(FbxFileTexture* texture, const fs::path& modelFolder,
			const fs::path& extractedFolder)
		{
			const fs::path absolute(FromUtf8(texture->GetFileName()));
			const fs::path relative(FromUtf8(texture->GetRelativeFileName()));
			const fs::path candidates[] = {
				absolute.is_absolute() ? absolute : modelFolder / absolute,
				modelFolder / relative,
				extractedFolder / relative,
				extractedFolder / relative.filename(),
				extractedFolder / absolute.filename(),
				modelFolder / relative.filename(),
				modelFolder / absolute.filename()
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
			ModelData& data;
			std::wstring& error;
			fs::path modelFolder;
			fs::path extractedFolder;
			std::unordered_map<FbxSurfaceMaterial*, uint32_t> materialIndices;
			std::unordered_map<std::wstring, uint32_t> textureIndices;
			std::vector<std::string> materialUvSets;
			bool hasBounds = false;

			bool LoadMap(FbxProperty property, TextureSlot slot, MaterialData& material,
				std::string& uvSet)
			{
				if (!property.IsValid()) return true;
				if (property.GetSrcObjectCount<FbxLayeredTexture>() != 0 ||
					property.GetSrcObjectCount<FbxFileTexture>() != 1)
				{
					error = L"재질에 겹친 텍스처 또는 지원하지 않는 텍스처가 있습니다: " +
						FromUtf8(material.name.c_str()) + L" / " + FromUtf8(property.GetName().Buffer());
					return false;
				}
				FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>();
				// 지금 Vertex에는 UV가 하나다. 조용히 다른 UV/변환을 적용하지 않는다.
				const std::string requested = texture->UVSet.Get().Buffer();
				if (!requested.empty() && requested != "default")
				{
					if (!uvSet.empty() && uvSet != requested)
					{
						error = L"한 재질이 서로 다른 UV 세트를 사용합니다: " + FromUtf8(material.name.c_str());
						return false;
					}
					uvSet = requested;
				}
				if (texture->GetMappingType() != FbxTexture::eUV || texture->GetSwapUV() ||
					std::fabs(texture->GetTranslationU()) > 1.0e-8 ||
					std::fabs(texture->GetTranslationV()) > 1.0e-8 ||
					std::fabs(texture->GetScaleU() - 1.0) > 1.0e-8 ||
					std::fabs(texture->GetScaleV() - 1.0) > 1.0e-8 ||
					std::fabs(texture->GetRotationU()) > 1.0e-8 ||
					std::fabs(texture->GetRotationV()) > 1.0e-8 ||
					std::fabs(texture->GetRotationW()) > 1.0e-8)
				{
					error = L"기본 UV 매핑만 지원합니다. 텍스처 변환을 모델 UV에 적용해 주세요: " +
						FromUtf8(texture->GetName());
					return false;
				}
				const fs::path path = FindTextureFile(texture, modelFolder, extractedFolder);
				if (path.empty())
				{
					error = L"FBX 참조 텍스처를 찾지 못했습니다: " + FromUtf8(texture->GetFileName()) +
						L"\n내장 텍스처 추출 폴더: " + extractedFolder.wstring();
					return false;
				}
				const bool srgb = slot == TextureSlot::BaseColor || slot == TextureSlot::Emissive;
				const bool normal = slot == TextureSlot::Normal;
				const std::wstring key = path.wstring() + (srgb ? L"|srgb" : normal ? L"|normal" : L"|linear");
				if (auto it = textureIndices.find(key); it != textureIndices.end())
				{
					material.textures[size_t(slot)] = it->second;
					return true;
				}
				TextureData image;
				if (!LoadTextureImage(path.c_str(), srgb, image, error)) return false;
				if (normal)
				{
					// Meshy/Blender의 +Y(OpenGL) 노멀 맵. 아래의 V 반전과 같이 처리한다.
					// 접선은 반전 후 UV로 계산하므로 녹색도 반전하며 셰이더는 다시 뒤집지 않는다.
					// 추후 -Y(DirectX) 원본을 받으면 이 변환을 로드 옵션으로 분리한다.
					for (size_t i = 1; i < image.pixels.size(); i += 4)
						image.pixels[i] = uint8_t(255 - image.pixels[i]);
				}
				const uint32_t index = uint32_t(data.textures.size());
				data.textures.push_back(std::move(image));
				textureIndices.emplace(key, index);
				material.textures[size_t(slot)] = index;
				return true;
			}

			bool LoadMaterial(FbxSurfaceMaterial* source, uint32_t& index)
			{
				if (auto it = materialIndices.find(source); it != materialIndices.end())
				{
					index = it->second;
					return true;
				}
				MaterialData material;
				std::string uvSet;
				if (source)
				{
					material.name = source->GetName();
					const XMFLOAT3 color = Color(source, { "BaseColor", "DiffuseColor" }, { 1, 1, 1 });
					const float diffuse = Scalar(source, { "DiffuseFactor" }, 1.0f);
					material.baseColor = { color.x * diffuse, color.y * diffuse, color.z * diffuse,
						std::clamp(1.0f - Scalar(source, { "TransparencyFactor" }, 0.0f), 0.0f, 1.0f) };
					material.emissive = Color(source, { "EmissiveColor" }, {});
					const float emission = Scalar(source, { "EmissiveFactor" }, 1.0f);
					material.emissive.x *= emission;
					material.emissive.y *= emission;
					material.emissive.z *= emission;
					const float shininess = std::max(0.0f, Scalar(source, { "ShininessExponent", "Shininess" }, 2.7f));
					material.roughness = std::clamp(Scalar(source, { "Roughness" },
						std::sqrt(2.0f / (shininess + 2.0f))), 0.04f, 1.0f);
					material.metallic = std::clamp(Scalar(source, { "Metalness", "Metallic", "ReflectionFactor" }, 0.0f), 0.0f, 1.0f);

					FbxProperty roughness = FindProperty(source, { "Roughness" }, true);
					if (!roughness.IsValid()) roughness = ExportedPbrMap(source, "ShininessExponent", "roughness_texture");
					FbxProperty metallic = FindProperty(source, { "Metalness", "Metallic" }, true);
					if (!metallic.IsValid()) metallic = ExportedPbrMap(source, "ReflectionFactor", "metallic_texture");
					if (!LoadMap(FindProperty(source, { "BaseColor", "DiffuseColor" }, true), TextureSlot::BaseColor, material, uvSet) ||
						!LoadMap(FindProperty(source, { "NormalMap" }, true), TextureSlot::Normal, material, uvSet) ||
						!LoadMap(roughness, TextureSlot::Roughness, material, uvSet) ||
						!LoadMap(metallic, TextureSlot::Metallic, material, uvSet) ||
						!LoadMap(FindProperty(source, { "EmissiveColor" }, true), TextureSlot::Emissive, material, uvSet)) return false;
					if (material.textures[size_t(TextureSlot::Roughness)] != kInvalidModelIndex) material.roughness = 1.0f;
					if (material.textures[size_t(TextureSlot::Metallic)] != kInvalidModelIndex) material.metallic = 1.0f;
				}
				index = uint32_t(data.materials.size());
				data.materials.push_back(std::move(material));
				materialUvSets.push_back(std::move(uvSet));
				materialIndices.emplace(source, index);
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

		bool PolygonMaterial(FbxMesh* mesh, FbxNode* node, int polygon, FbxSurfaceMaterial*& material)
		{
			material = nullptr;
			if (node->GetMaterialCount() == 0) return true;
			FbxGeometryElementMaterial* element = mesh->GetElementMaterial();
			int index = 0;
			if (element)
			{
				switch (element->GetMappingMode())
				{
				case FbxLayerElement::eAllSame: break;
				case FbxLayerElement::eByPolygon: index = polygon; break;
				default: return false;
				}
				// Material의 IndexToDirect 값은 이 노드의 재질 슬롯 번호다.
				if (element->GetReferenceMode() == FbxLayerElement::eIndexToDirect ||
					element->GetReferenceMode() == FbxLayerElement::eIndex)
				{
					if (index >= element->GetIndexArray().GetCount()) return false;
					index = element->GetIndexArray().GetAt(index);
				}
			}
			if (index < 0) return true;
			if (index >= node->GetMaterialCount()) return false;
			material = node->GetMaterial(index);
			return true;
		}

		bool VertexColor(FbxMesh* mesh, int polygon, int corner, int controlPoint, XMFLOAT3& color)
		{
			const FbxGeometryElementVertexColor* element = mesh->GetElementVertexColor();
			if (!element) return true;
			int index = 0;
			switch (element->GetMappingMode())
			{
			case FbxLayerElement::eByControlPoint: index = controlPoint; break;
			case FbxLayerElement::eByPolygonVertex: index = mesh->GetPolygonVertexIndex(polygon) + corner; break;
			case FbxLayerElement::eByPolygon: index = polygon; break;
			case FbxLayerElement::eAllSame: break;
			default: return false;
			}
			if (element->GetReferenceMode() == FbxLayerElement::eIndexToDirect ||
				element->GetReferenceMode() == FbxLayerElement::eIndex)
			{
				if (index < 0 || index >= element->GetIndexArray().GetCount()) return false;
				index = element->GetIndexArray().GetAt(index);
			}
			if (index < 0 || index >= element->GetDirectArray().GetCount()) return false;
			const FbxColor value = element->GetDirectArray().GetAt(index);
			color = { float(value.mRed), float(value.mGreen), float(value.mBlue) };
			return Finite(color);
		}

		void MakeTangents(Vertex (&vertices)[3])
		{
			const XMVECTOR edge1 = XMLoadFloat3(&vertices[1].position) - XMLoadFloat3(&vertices[0].position);
			const XMVECTOR edge2 = XMLoadFloat3(&vertices[2].position) - XMLoadFloat3(&vertices[0].position);
			const float u1 = vertices[1].uv.x - vertices[0].uv.x;
			const float v1 = vertices[1].uv.y - vertices[0].uv.y;
			const float u2 = vertices[2].uv.x - vertices[0].uv.x;
			const float v2 = vertices[2].uv.y - vertices[0].uv.y;
			const float determinant = u1 * v2 - u2 * v1;
			const bool validUv = std::fabs(determinant) > 1.0e-12f;
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

		bool LoadMesh(FbxMesh* source, FbxNode* node, uint32_t nodeIndex,
			const XMFLOAT4X4& global, ImportContext& context)
		{
			FbxAMatrix geometry;
			geometry.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
			geometry.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
			geometry.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));
			const XMFLOAT4X4 geometryMatrix = ToMatrix(geometry);
			if (!Invertible(geometryMatrix))
			{
				context.error = L"FBX 메시의 기하 변환이 유효하지 않습니다: " + FromUtf8(node->GetName());
				return false;
			}
			const XMMATRIX geometric = XMLoadFloat4x4(&geometryMatrix);
			const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, geometric));
			const XMMATRIX world = XMLoadFloat4x4(&global);
			const float worldSign = XMVectorGetX(XMMatrixDeterminant(world)) < 0 ? -1.0f : 1.0f;
			FbxStringList uvNames;
			source->GetUVSetNames(uvNames);
			std::unordered_map<uint32_t, uint32_t> submeshes;
			for (int polygon = 0; polygon < source->GetPolygonCount(); ++polygon)
			{
				if (source->GetPolygonSize(polygon) != 3)
				{
					context.error = L"FBX 삼각형 변환 후에도 삼각형이 아닌 면이 남았습니다.";
					return false;
				}
				FbxSurfaceMaterial* material = nullptr;
				uint32_t materialIndex = kInvalidModelIndex;
				if (!PolygonMaterial(source, node, polygon, material))
				{
					context.error = L"FBX 면의 재질 인덱스를 읽지 못했습니다: " + FromUtf8(node->GetName());
					return false;
				}
				if (!context.LoadMaterial(material, materialIndex)) return false;
				const std::string& requestedUv = context.materialUvSets[materialIndex];
				const char* uvName = requestedUv.empty() ? (uvNames.GetCount() > 0 ? uvNames.GetStringAt(0) : nullptr) : requestedUv.c_str();
				const MaterialData& targetMaterial = context.data.materials[materialIndex];
				const bool textured = std::any_of(targetMaterial.textures.begin(), targetMaterial.textures.end(),
					[](uint32_t index) { return index != kInvalidModelIndex; });
				Vertex vertices[3];
				for (int corner = 0; corner < 3; ++corner)
				{
					const int controlPoint = source->GetPolygonVertex(polygon, corner);
					if (controlPoint < 0 || controlPoint >= source->GetControlPointsCount())
					{
						context.error = L"FBX 정점 인덱스가 범위를 벗어났습니다.";
						return false;
					}
					const FbxVector4 position = source->GetControlPointAt(controlPoint);
					const XMVECTOR point = XMVector3TransformCoord(XMVectorSet(float(position[0]), float(position[1]), float(position[2]), 1), geometric);
					XMStoreFloat3(&vertices[corner].position, point);
					FbxVector4 normal;
					if (!source->GetPolygonVertexNormal(polygon, corner, normal))
					{
						context.error = L"FBX 정점 법선을 읽지 못했습니다.";
						return false;
					}
					const XMVECTOR transformedNormal = XMVector3TransformNormal(
						XMVectorSet(float(normal[0]), float(normal[1]), float(normal[2]), 0), normalMatrix);
					if (XMVectorGetX(XMVector3LengthSq(transformedNormal)) < 1.0e-20f)
					{
						context.error = L"FBX 정점 법선의 길이가 0입니다.";
						return false;
					}
					XMStoreFloat3(&vertices[corner].normal, XMVector3Normalize(transformedNormal));
					if (!Finite(vertices[corner].position) || !Finite(vertices[corner].normal) ||
						!VertexColor(source, polygon, corner, controlPoint, vertices[corner].color))
					{
						context.error = L"FBX 정점 데이터가 유효하지 않습니다.";
						return false;
					}
					FbxVector2 uv;
					bool unmapped = false;
					const bool hasUv = uvName && source->GetPolygonVertexUV(polygon, corner, uvName, uv, unmapped) && !unmapped;
					if (hasUv)
						vertices[corner].uv = { float(uv[0]), 1.0f - float(uv[1]) };
					if ((textured && !hasUv) || !std::isfinite(vertices[corner].uv.x) || !std::isfinite(vertices[corner].uv.y))
					{
						context.error = L"텍스처가 있는 FBX 면의 UV를 읽지 못했습니다: " + FromUtf8(node->GetName());
						return false;
					}
				}
				const XMVECTOR edge1 = XMLoadFloat3(&vertices[1].position) - XMLoadFloat3(&vertices[0].position);
				const XMVECTOR edge2 = XMLoadFloat3(&vertices[2].position) - XMLoadFloat3(&vertices[0].position);
				const XMVECTOR faceNormal = XMVector3Cross(edge1, edge2);
				if (XMVectorGetX(XMVector3LengthSq(faceNormal)) < 1.0e-20f) continue;
				const XMVECTOR averageNormal = XMLoadFloat3(&vertices[0].normal) + XMLoadFloat3(&vertices[1].normal) + XMLoadFloat3(&vertices[2].normal);
				// D3D의 기본 CW 전면. SDK handedness 변환에 다시 무조건 반전을 겹치지 않는다.
				if (worldSign * XMVectorGetX(XMVector3Dot(faceNormal, averageNormal)) < 0)
					std::swap(vertices[1], vertices[2]);
				MakeTangents(vertices);
				uint32_t meshIndex;
				if (auto it = submeshes.find(materialIndex); it != submeshes.end()) meshIndex = it->second;
				else
				{
					meshIndex = uint32_t(context.data.meshes.size());
					context.data.meshes.push_back({ {}, materialIndex });
					context.data.nodes[nodeIndex].meshes.push_back(meshIndex);
					submeshes.emplace(materialIndex, meshIndex);
				}
				MeshData& mesh = context.data.meshes[meshIndex].mesh;
				if (mesh.vertices.size() > size_t(UINT32_MAX) - 3)
				{
					context.error = L"FBX 메시가 32비트 정점 인덱스 범위를 넘었습니다.";
					return false;
				}
				// 제어점이 같아도 UV/법선이 다를 수 있어 면 모서리별로 정점을 만든다.
				for (const Vertex& vertex : vertices)
				{
					mesh.indices.push_back(uint32_t(mesh.vertices.size()));
					mesh.vertices.push_back(vertex);
					XMFLOAT3 modelPosition;
					XMStoreFloat3(&modelPosition, XMVector3TransformCoord(XMLoadFloat3(&vertex.position), world));
					if (!Finite(modelPosition))
					{
						context.error = L"FBX 모델 경계 계산 중 유효하지 않은 좌표가 나왔습니다.";
						return false;
					}
					context.IncludePoint(modelPosition);
				}
			}
			return true;
		}

		bool LoadNode(FbxNode* source, uint32_t parent, const XMFLOAT4X4& parentGlobal,
			ImportContext& context)
		{
			// 기본 자세를 읽는다. 애니메이션 재생/스키닝은 다음 단계에서 추가한다.
			const XMFLOAT4X4 global = ToMatrix(source->EvaluateGlobalTransform());
			if (!Invertible(global))
			{
				context.error = L"FBX 노드 변환이 유효하지 않습니다: " + FromUtf8(source->GetName());
				return false;
			}
			ModelNodeData node;
			node.name = source->GetName();
			node.parent = parent;
			// RrSs/Rrs 상속과 피벗을 SDK가 평가한 결과에서 local을 복원한다.
			XMStoreFloat4x4(&node.local, XMLoadFloat4x4(&global) *
				XMMatrixInverse(nullptr, XMLoadFloat4x4(&parentGlobal)));
			const uint32_t index = uint32_t(context.data.nodes.size());
			context.data.nodes.push_back(std::move(node));
			for (int i = 0; i < source->GetNodeAttributeCount(); ++i)
			{
				FbxNodeAttribute* attribute = source->GetNodeAttributeByIndex(i);
				if (attribute && attribute->GetAttributeType() == FbxNodeAttribute::eMesh &&
					!LoadMesh(static_cast<FbxMesh*>(attribute), source, index, global, context)) return false;
			}
			for (int i = 0; i < source->GetChildCount(); ++i)
				if (!LoadNode(source->GetChild(i), index, global, context)) return false;
			return true;
		}
	}

	bool FbxModelLoader::Load(const wchar_t* path, ModelData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path)
		{
			error = L"FBX 파일 경로가 비어 있습니다.";
			return false;
		}
		std::error_code ec;
		const fs::path modelPath = fs::absolute(fs::path(path), ec).lexically_normal();
		if (ec || !fs::is_regular_file(modelPath, ec))
		{
			error = L"FBX 파일을 찾지 못했습니다: " + std::wstring(path);
			return false;
		}
		const std::string filename = ToUtf8(modelPath.wstring());
		if (filename.empty())
		{
			error = L"FBX 파일 경로를 UTF-8로 변환하지 못했습니다.";
			return false;
		}
		// FBX SDK의 내장 이미지 추출은 실행 중 발생한다. 원본 assets 폴더를 수정하지 않는다.
		fs::path extracted = fs::temp_directory_path(ec);
		if (ec)
		{
			error = L"FBX 내장 텍스처의 임시 폴더를 찾지 못했습니다.";
			return false;
		}
		const auto stamp = fs::last_write_time(modelPath, ec);
		if (ec)
		{
			error = L"FBX 파일 수정 시간을 읽지 못했습니다.";
			return false;
		}
		const std::wstring cacheKey = modelPath.wstring() + std::to_wstring(stamp.time_since_epoch().count());
		extracted /= fs::path(L"SpaceWar") / L"FbxCache" / std::to_wstring(std::hash<std::wstring>{}(cacheKey));
		fs::create_directories(extracted, ec);
		if (ec)
		{
			error = L"FBX 내장 텍스처를 추출할 폴더를 만들지 못했습니다: " + extracted.wstring();
			return false;
		}
		const std::string extractionPath = ToUtf8(extracted.wstring());
		if (extractionPath.empty())
		{
			error = L"FBX 텍스처 추출 경로를 UTF-8로 변환하지 못했습니다.";
			return false;
		}
		std::unique_ptr<FbxManager, FbxDestroy> manager(FbxManager::Create());
		if (!manager)
		{
			error = L"FBX SDK 매니저 생성 실패";
			return false;
		}
		FbxIOSettings* settings = FbxIOSettings::Create(manager.get(), IOSROOT);
		FbxScene* scene = FbxScene::Create(manager.get(), "Model");
		std::unique_ptr<FbxImporter, FbxDestroy> importer(FbxImporter::Create(manager.get(), "ModelImporter"));
		if (!settings || !scene || !importer)
		{
			error = L"FBX SDK 로더 객체 생성 실패";
			return false;
		}
		manager->SetIOSettings(settings);
		settings->SetBoolProp(IMP_FBX_MATERIAL, true);
		settings->SetBoolProp(IMP_FBX_TEXTURE, true);
		settings->SetBoolProp(IMP_FBX_ANIMATION, false);
		if (!importer->Initialize(filename.c_str(), -1, settings))
		{
			error = L"FBX 초기화 실패: " + FromUtf8(importer->GetStatus().GetErrorString());
			return false;
		}
		importer->SetEmbeddingExtractionFolder(extractionPath.c_str());
		if (!importer->Import(scene))
		{
			error = L"FBX 읽기 실패: " + FromUtf8(importer->GetStatus().GetErrorString());
			return false;
		}
		importer.reset();
		FbxGeometryConverter converter(manager.get());
		if (!converter.Triangulate(scene, true))
		{
			error = L"FBX 메시를 삼각형으로 변환하지 못했습니다.";
			return false;
		}
		for (int i = 0; i < scene->GetSrcObjectCount<FbxMesh>(); ++i)
		{
			FbxMesh* mesh = scene->GetSrcObject<FbxMesh>(i);
			if (mesh->GetPolygonCount() > 0 && !mesh->GenerateNormals(false, false, false))
			{
				error = L"FBX 메시 법선 생성 실패";
				return false;
			}
		}
		// ConvertScene은 회전만 처리한다. 좌/우수 변경에는 DeepConvertScene이 필요하다.
		FbxAxisSystem::DirectX.DeepConvertScene(scene);
		FbxSystemUnit::m.ConvertScene(scene);
		ModelData data;
		ImportContext context{ data, error, modelPath.parent_path(), extracted };
		XMFLOAT4X4 identity;
		XMStoreFloat4x4(&identity, XMMatrixIdentity());
		if (!scene->GetRootNode() || !LoadNode(scene->GetRootNode(), kInvalidModelIndex, identity, context))
		{
			if (error.empty()) error = L"FBX 루트 노드를 읽지 못했습니다.";
			return false;
		}
		if (data.meshes.empty() || !context.hasBounds)
		{
			error = L"FBX 파일에 표시할 삼각형 메시가 없습니다.";
			return false;
		}
		out = std::move(data);
		return true;
	}
}
