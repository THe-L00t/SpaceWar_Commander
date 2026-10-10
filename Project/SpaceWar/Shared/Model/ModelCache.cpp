#include "ModelCache.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include <type_traits>

namespace Shared {

	namespace {

		namespace fs = std::filesystem;

		constexpr char kMagic[4] = { 'S', 'W', 'M', 'C' };
		constexpr char kEndMagic[4] = { 'S', 'W', 'M', 'E' };   // 끝까지 썼는지 확인용

		enum OptionBit : uint32_t
		{
			kGeometryOnly = 1u << 0,
			kGenerateCollision = 1u << 1,
			kGenerateTangents = 1u << 2,
			kReadAnimation = 1u << 3,
			kSplitByObject = 1u << 4,
			kCollectObject = 1u << 5,
		};

		uint64_t Fnv1a(const void* data, size_t size, uint64_t hash = 1469598103934665603ull)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		// ── 쓰기 ────────────────────────────────────────────
		//  배열은 «개수(u64) + 원소 바이트 그대로» 다. 원소는 전부 trivially copyable 이어야 한다.
		class Writer
		{
		public:
			explicit Writer(FILE* file) : file(file) {}

			bool ok = true;

			void Bytes(const void* data, size_t size)
			{
				if (ok && size != 0 && std::fwrite(data, 1, size, file) != size) ok = false;
			}

			template <class T>
			void Pod(const T& value)
			{
				static_assert(std::is_trivially_copyable_v<T>, "그대로 쓸 수 있는 타입만");
				Bytes(&value, sizeof(T));
			}

			void Count(size_t count) { Pod(static_cast<uint64_t>(count)); }

			void String(const std::string& text)
			{
				Count(text.size());
				Bytes(text.data(), text.size());
			}

			void WString(const std::wstring& text)
			{
				Count(text.size());
				Bytes(text.data(), text.size() * sizeof(wchar_t));
			}

			template <class T>
			void Array(const std::vector<T>& values)
			{
				static_assert(std::is_trivially_copyable_v<T>, "그대로 쓸 수 있는 타입만");
				Count(values.size());
				Bytes(values.data(), values.size() * sizeof(T));
			}

		private:
			FILE* file;
		};

		// ── 읽기 ────────────────────────────────────────────
		//  ★ 개수를 믿고 먼저 할당하지 않는다 — 남은 파일 크기보다 큰 배열이면 깨진 캐시로 본다.
		class Reader
		{
		public:
			Reader(FILE* file, uint64_t size) : file(file), remaining(size) {}

			bool ok = true;

			void Bytes(void* data, size_t size)
			{
				if (!ok) return;
				if (size > remaining) { ok = false; return; }
				if (size != 0 && std::fread(data, 1, size, file) != size) { ok = false; return; }
				remaining -= size;
			}

			template <class T>
			void Pod(T& value)
			{
				static_assert(std::is_trivially_copyable_v<T>, "그대로 읽을 수 있는 타입만");
				Bytes(&value, sizeof(T));
			}

			// 원소 하나가 최소 minBytes 를 차지한다고 보고 개수를 검사한다.
			bool Count(size_t minBytes, size_t& count)
			{
				uint64_t raw = 0;
				Pod(raw);
				if (!ok) return false;
				if (minBytes != 0 && raw > remaining / minBytes) { ok = false; return false; }
				count = static_cast<size_t>(raw);
				return true;
			}

			void String(std::string& text)
			{
				size_t count = 0;
				if (!Count(1, count)) return;
				text.resize(count);
				Bytes(text.data(), count);
			}

			void WString(std::wstring& text)
			{
				size_t count = 0;
				if (!Count(sizeof(wchar_t), count)) return;
				text.resize(count);
				Bytes(text.data(), count * sizeof(wchar_t));
			}

			template <class T>
			void Array(std::vector<T>& values)
			{
				static_assert(std::is_trivially_copyable_v<T>, "그대로 읽을 수 있는 타입만");
				size_t count = 0;
				if (!Count(sizeof(T), count)) return;
				values.resize(count);
				Bytes(values.data(), count * sizeof(T));
			}

			bool AtEnd() const { return remaining == 0; }

		private:
			FILE*    file;
			uint64_t remaining;
		};

		// ── 머리말 ──────────────────────────────────────────
		void WriteHeader(Writer& w, const ModelCacheKey& key)
		{
			w.Bytes(kMagic, sizeof(kMagic));
			w.Pod(kModelCacheVersion);
			w.Pod(static_cast<uint32_t>(sizeof(Vertex)));
			w.Pod(static_cast<uint32_t>(sizeof(SkinVertex)));
			w.Pod(key.sourceSize);
			w.Pod(key.sourceTime);
			w.Pod(key.optionFlags);
			w.WString(key.sourcePath);
			w.String(key.tag);
		}

		bool ReadHeaderMatches(Reader& r, const ModelCacheKey& key)
		{
			char magic[4] = {};
			uint32_t version = 0, vertexSize = 0, skinSize = 0, optionFlags = 0;
			uint64_t sourceSize = 0;
			int64_t sourceTime = 0;
			std::wstring sourcePath;
			std::string tag;

			r.Bytes(magic, sizeof(magic));
			r.Pod(version);
			r.Pod(vertexSize);
			r.Pod(skinSize);
			r.Pod(sourceSize);
			r.Pod(sourceTime);
			r.Pod(optionFlags);
			r.WString(sourcePath);
			r.String(tag);

			return r.ok &&
				std::memcmp(magic, kMagic, sizeof(kMagic)) == 0 &&
				version == kModelCacheVersion &&
				vertexSize == sizeof(Vertex) &&
				skinSize == sizeof(SkinVertex) &&
				sourceSize == key.sourceSize &&
				sourceTime == key.sourceTime &&
				optionFlags == key.optionFlags &&
				sourcePath == key.sourcePath &&
				tag == key.tag;
		}

		// ── 본문 ────────────────────────────────────────────
		//  쓰는 순서와 읽는 순서가 정확히 같아야 한다. 필드를 더하면 kModelCacheVersion 을 올린다.
		void WriteBody(Writer& w, const ModelSource& s)
		{
			w.Count(s.meshes.size());
			for (const SourceMesh& mesh : s.meshes)
			{
				w.Array(mesh.vertices);
				w.Array(mesh.indices);
				w.Pod(mesh.material);
				w.Array(mesh.skin);
			}

			w.Count(s.materials.size());
			for (const SourceMaterial& material : s.materials)
			{
				w.String(material.name);
				w.Pod(material.baseColor);
				w.Pod(material.emissive);
				w.Pod(material.roughness);
				w.Pod(material.metallic);
				for (const std::string& path : material.texturePaths) w.String(path);
				w.Pod(material.embeddedImages);
			}

			w.Count(s.images.size());
			for (const SourceImage& image : s.images)
			{
				w.String(image.name);
				w.String(image.mimeType);
				w.Array(image.bytes);
			}

			w.Count(s.nodes.size());
			for (const SourceNode& node : s.nodes)
			{
				w.String(node.name);
				w.Pod(node.parent);
				w.Pod(node.local);
				w.Array(node.meshes);
			}

			w.String(s.skeleton.name);
			w.Count(s.skeleton.joints.size());
			for (const SourceJoint& joint : s.skeleton.joints)
			{
				w.String(joint.name);
				w.Pod(joint.parent);
				w.Pod(joint.localRest);
				w.Pod(joint.inverseBind);
			}

			w.Count(s.animations.size());
			for (const AnimationSource& clip : s.animations)
			{
				w.String(clip.name);
				w.Pod(clip.duration);
				w.Count(clip.channels.size());
				for (const AnimationChannel& channel : clip.channels)
				{
					w.Pod(channel.joint);
					w.Pod(channel.path);
					w.Pod(channel.interpolation);
					w.Array(channel.times);
					w.Array(channel.values);
				}
			}

			w.Array(s.collision.vertices);
			w.Array(s.collision.indices);
			w.Pod(s.collision.boundsMin);
			w.Pod(s.collision.boundsMax);

			w.Array(s.collected.positions);
			w.Array(s.collected.indices);
			w.Count(s.collected.objects.size());
			for (const std::string& name : s.collected.objects) w.String(name);

			w.Pod(s.boundsMin);
			w.Pod(s.boundsMax);
			w.Pod(s.format);
			w.WString(s.sourceDirectory);

			w.Bytes(kEndMagic, sizeof(kEndMagic));
		}

		// 원소 하나가 최소로 차지하는 바이트(개수 검사용). 문자열·배열은 개수 칸(8B)이 늘 있다.
		constexpr size_t kMinRecord = sizeof(uint64_t);

		bool ReadBody(Reader& r, ModelSource& s)
		{
			size_t count = 0;

			if (!r.Count(kMinRecord, count)) return false;
			s.meshes.resize(count);
			for (SourceMesh& mesh : s.meshes)
			{
				r.Array(mesh.vertices);
				r.Array(mesh.indices);
				r.Pod(mesh.material);
				r.Array(mesh.skin);
				if (!r.ok) return false;
			}

			if (!r.Count(kMinRecord, count)) return false;
			s.materials.resize(count);
			for (SourceMaterial& material : s.materials)
			{
				r.String(material.name);
				r.Pod(material.baseColor);
				r.Pod(material.emissive);
				r.Pod(material.roughness);
				r.Pod(material.metallic);
				for (std::string& path : material.texturePaths) r.String(path);
				r.Pod(material.embeddedImages);
				if (!r.ok) return false;
			}

			if (!r.Count(kMinRecord, count)) return false;
			s.images.resize(count);
			for (SourceImage& image : s.images)
			{
				r.String(image.name);
				r.String(image.mimeType);
				r.Array(image.bytes);
				if (!r.ok) return false;
			}

			if (!r.Count(kMinRecord, count)) return false;
			s.nodes.resize(count);
			for (SourceNode& node : s.nodes)
			{
				r.String(node.name);
				r.Pod(node.parent);
				r.Pod(node.local);
				r.Array(node.meshes);
				if (!r.ok) return false;
			}

			r.String(s.skeleton.name);
			if (!r.Count(kMinRecord, count)) return false;
			s.skeleton.joints.resize(count);
			for (SourceJoint& joint : s.skeleton.joints)
			{
				r.String(joint.name);
				r.Pod(joint.parent);
				r.Pod(joint.localRest);
				r.Pod(joint.inverseBind);
				if (!r.ok) return false;
			}

			if (!r.Count(kMinRecord, count)) return false;
			s.animations.resize(count);
			for (AnimationSource& clip : s.animations)
			{
				r.String(clip.name);
				r.Pod(clip.duration);
				size_t channelCount = 0;
				if (!r.Count(kMinRecord, channelCount)) return false;
				clip.channels.resize(channelCount);
				for (AnimationChannel& channel : clip.channels)
				{
					r.Pod(channel.joint);
					r.Pod(channel.path);
					r.Pod(channel.interpolation);
					r.Array(channel.times);
					r.Array(channel.values);
					if (!r.ok) return false;
				}
			}

			r.Array(s.collision.vertices);
			r.Array(s.collision.indices);
			r.Pod(s.collision.boundsMin);
			r.Pod(s.collision.boundsMax);

			r.Array(s.collected.positions);
			r.Array(s.collected.indices);
			if (!r.Count(kMinRecord, count)) return false;
			s.collected.objects.resize(count);
			for (std::string& name : s.collected.objects) r.String(name);

			r.Pod(s.boundsMin);
			r.Pod(s.boundsMax);
			r.Pod(s.format);
			r.WString(s.sourceDirectory);

			char endMagic[4] = {};
			r.Bytes(endMagic, sizeof(endMagic));
			return r.ok && std::memcmp(endMagic, kEndMagic, sizeof(kEndMagic)) == 0 && r.AtEnd();
		}

	} // namespace

	bool MakeModelCacheKey(const std::wstring& sourcePath, const ReadOptions& options,
		const std::string& tag, ModelCacheKey& out)
	{
		out = ModelCacheKey{};

		std::error_code ec;
		const fs::path full = fs::absolute(sourcePath, ec).lexically_normal();
		if (ec) return false;
		const uint64_t size = fs::file_size(full, ec);
		if (ec) return false;
		const fs::file_time_type time = fs::last_write_time(full, ec);
		if (ec) return false;

		out.sourcePath = full.wstring();
		out.sourceSize = size;
		out.sourceTime = static_cast<int64_t>(time.time_since_epoch().count());
		out.tag = tag;

		uint32_t flags = 0;
		if (options.geometryOnly)          flags |= kGeometryOnly;
		if (options.generateCollision)     flags |= kGenerateCollision;
		if (options.generateTangents)      flags |= kGenerateTangents;
		if (options.readAnimation)         flags |= kReadAnimation;
		if (options.splitByObject)         flags |= kSplitByObject;
		if (options.collectObject)         flags |= kCollectObject;
		out.optionFlags = flags;
		return true;
	}

	std::wstring ModelCacheFileName(const ModelCacheKey& key)
	{
		uint64_t hash = Fnv1a(key.sourcePath.data(), key.sourcePath.size() * sizeof(wchar_t));
		hash = Fnv1a(&key.optionFlags, sizeof(key.optionFlags), hash);
		hash = Fnv1a(key.tag.data(), key.tag.size(), hash);

		wchar_t hex[17] = {};
		std::swprintf(hex, 17, L"%016llx", static_cast<unsigned long long>(hash));
		return fs::path(key.sourcePath).stem().wstring() + L"_" + hex + L".swmc";
	}

	bool ReadModelCache(const std::wstring& cachePath, const ModelCacheKey& key, ModelSource& out)
	{
		out = ModelSource{};

		std::error_code ec;
		const uint64_t size = fs::file_size(cachePath, ec);
		if (ec) return false;

		FILE* file = nullptr;
		if (::_wfopen_s(&file, cachePath.c_str(), L"rb") != 0 || !file)
			return false;

		Reader reader(file, size);
		bool ok = false;
		try
		{
			ok = ReadHeaderMatches(reader, key) && ReadBody(reader, out);
		}
		catch (...)
		{
			ok = false;   // 깨진 캐시의 거대한 개수 등 — 그냥 다시 파싱한다
		}
		std::fclose(file);

		if (!ok) out = ModelSource{};
		return ok;
	}

	bool WriteModelCache(const std::wstring& cachePath, const ModelCacheKey& key,
		const ModelSource& source, std::wstring& error)
	{
		std::error_code ec;
		const fs::path target(cachePath);
		fs::create_directories(target.parent_path(), ec);
		if (ec)
		{
			error = L"캐시 폴더를 만들 수 없습니다: " + target.parent_path().wstring();
			return false;
		}

		// 임시 파일에 다 쓴 뒤 이름을 바꾼다 — 쓰다 끊겨도 반쪽 캐시가 남지 않는다.
		const fs::path temp = target.wstring() + L".tmp";
		FILE* file = nullptr;
		if (::_wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file)
		{
			error = L"캐시 파일을 열 수 없습니다: " + temp.wstring();
			return false;
		}

		Writer writer(file);
		WriteHeader(writer, key);
		WriteBody(writer, source);
		const bool closed = std::fclose(file) == 0;

		if (!writer.ok || !closed)
		{
			fs::remove(temp, ec);
			error = L"캐시 파일을 끝까지 쓰지 못했습니다: " + temp.wstring();
			return false;
		}

		fs::rename(temp, target, ec);   // 같은 이름의 낡은 캐시는 덮어쓴다
		if (ec)
		{
			fs::remove(temp, ec);
			error = L"캐시 파일 이름을 바꾸지 못했습니다: " + target.wstring();
			return false;
		}
		return true;
	}

} // namespace Shared
