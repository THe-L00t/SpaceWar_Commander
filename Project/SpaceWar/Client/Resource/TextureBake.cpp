#include "TextureBake.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>

namespace swc {

	namespace {

		namespace fs = std::filesystem;

		constexpr char kMagic[4] = { 'S', 'W', 'T', 'X' };
		constexpr char kEndMagic[4] = { 'S', 'W', 'T', 'E' };   // 끝까지 썼는지 확인용

		// 파일 머리말 — 그대로 쓰고 그대로 읽는다(같은 기계 전용). 빈틈 없는 배치(56 바이트).
		struct BakeHeader
		{
			char     magic[4];
			uint32_t version;
			uint64_t hash;
			uint64_t sourceBytes;
			uint32_t width;
			uint32_t height;
			uint32_t srgb;
			uint32_t format;      // TextureFormat
			uint32_t mipLevels;
			uint32_t arraySize;
			uint64_t pixelBytes;
		};
		static_assert(sizeof(BakeHeader) == 56, "머리말 배치가 바뀌면 kTextureBakeVersion 을 올린다");

		// splitmix64 마무리 — 해시 비트를 고르게 섞는다.
		uint64_t Mix(uint64_t x)
		{
			x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
			x ^= x >> 27; x *= 0x94D049BB133111EBull;
			x ^= x >> 31;
			return x;
		}

		// 파일 이름에 쓸 수 있는 글자만 남긴다(영문 · 숫자 · _ · -), 길이는 64 자까지.
		std::wstring SafeName(const std::wstring& name)
		{
			std::wstring out;
			out.reserve((std::min)(name.size(), size_t(64)));
			for (wchar_t c : name)
			{
				if (out.size() >= 64) break;
				const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
					(c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
				out.push_back(ok ? c : L'_');
			}
			return out.empty() ? std::wstring(L"texture") : out;
		}

	} // namespace

	TextureBakeKey MakeTextureBakeKey(const uint8_t* bytes, size_t size, bool srgb)
	{
		// 8 바이트씩 섞는다 — 바이트마다 도는 FNV 보다 빠르다(행성 하나의 PNG 가 수백 MB).
		// 충돌을 노리는 입력이 아니고, 바이트 수 · sRGB 도 키에 따로 들어간다.
		uint64_t h = 0x9E3779B97F4A7C15ull ^ uint64_t(size);
		size_t i = 0;
		for (; i + 8 <= size; i += 8)
		{
			uint64_t word = 0;
			std::memcpy(&word, bytes + i, 8);
			h ^= word;
			h *= 0x9E3779B97F4A7C15ull;   // 홀수 곱(황금비 상수)
			h ^= h >> 29;
		}
		uint64_t tail = 0;
		if (i < size) std::memcpy(&tail, bytes + i, size - i);
		h = Mix(h ^ tail);

		TextureBakeKey key;
		key.hash = h;
		key.sourceBytes = size;
		key.srgb = srgb;
		return key;
	}

	std::wstring TextureBakePath(const std::wstring& directory, const std::wstring& name, const TextureBakeKey& key)
	{
		wchar_t hex[17] = {};
		std::swprintf(hex, 17, L"%016llx", static_cast<unsigned long long>(key.hash));
		return (fs::path(directory) / (SafeName(name) + L"_" + hex + (key.srgb ? L"_s" : L"_l") + L".swtx")).wstring();
	}

	bool ReadTextureBake(const std::wstring& path, const TextureBakeKey& key, TextureData& out)
	{
		std::FILE* file = nullptr;
		if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
			return false;

		BakeHeader header{};
		bool ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
			std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 &&
			header.version == kTextureBakeVersion &&
			header.hash == key.hash &&
			header.sourceBytes == key.sourceBytes &&
			header.srgb == (key.srgb ? 1u : 0u) &&
			header.format == static_cast<uint32_t>(TextureFormat::Rgba8) &&
			header.mipLevels == 1 && header.arraySize == 1 &&
			header.width > 0 && header.height > 0 && header.width <= 16384 && header.height <= 16384 &&
			header.pixelBytes == uint64_t(header.width) * header.height * 4;

		TextureData data;
		if (ok)
		{
			data.pixels.resize(static_cast<size_t>(header.pixelBytes));
			ok = std::fread(data.pixels.data(), 1, data.pixels.size(), file) == data.pixels.size();
			char endMagic[4] = {};
			ok = ok && std::fread(endMagic, 1, sizeof(endMagic), file) == sizeof(endMagic) &&
				std::memcmp(endMagic, kEndMagic, sizeof(kEndMagic)) == 0 &&
				std::fgetc(file) == EOF;   // 뒤에 남는 바이트가 없어야 한다
		}
		std::fclose(file);
		if (!ok) return false;

		data.path = path;
		data.width = header.width;
		data.height = header.height;
		data.srgb = key.srgb;
		data.format = TextureFormat::Rgba8;
		data.mipLevels = 1;
		data.arraySize = 1;
		out = std::move(data);
		return true;
	}

	bool WriteTextureBake(const std::wstring& path, const TextureBakeKey& key, const TextureData& data)
	{
		if (data.format != TextureFormat::Rgba8 || data.mipLevels != 1 || data.arraySize != 1 ||
			data.width == 0 || data.height == 0 ||
			data.pixels.size() != size_t(data.width) * data.height * 4)
			return false;

		std::error_code ec;
		const fs::path target(path);
		fs::create_directories(target.parent_path(), ec);
		if (ec) return false;

		// 임시 이름에 스레드 번호를 붙인다 — 로딩 스레드 여럿이 같은 이미지(공용 트림 등)를 동시에 구워도 서로 덮지 않는다.
		const fs::path temp = target.wstring() + L".tmp" +
			std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id()));

		std::FILE* file = nullptr;
		if (::_wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file)
			return false;

		BakeHeader header{};
		std::memcpy(header.magic, kMagic, sizeof(kMagic));
		header.version = kTextureBakeVersion;
		header.hash = key.hash;
		header.sourceBytes = key.sourceBytes;
		header.width = data.width;
		header.height = data.height;
		header.srgb = key.srgb ? 1u : 0u;
		header.format = static_cast<uint32_t>(TextureFormat::Rgba8);
		header.mipLevels = 1;
		header.arraySize = 1;
		header.pixelBytes = data.pixels.size();

		bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
			std::fwrite(data.pixels.data(), 1, data.pixels.size(), file) == data.pixels.size() &&
			std::fwrite(kEndMagic, 1, sizeof(kEndMagic), file) == sizeof(kEndMagic);
		ok = (std::fclose(file) == 0) && ok;

		if (ok)
		{
			// 다른 스레드가 먼저 같은 파일을 만들었으면(내용은 같다) 이름 바꾸기가 실패할 수 있다 — 괜찮다.
			fs::rename(temp, target, ec);
			ok = !ec;
		}
		if (!ok) fs::remove(temp, ec);
		return ok;
	}
}
