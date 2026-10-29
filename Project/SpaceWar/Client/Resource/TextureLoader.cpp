#include "TextureLoader.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace swc {

	namespace {

		bool CreateFactory(ComPtr<IWICImagingFactory>& factory, std::wstring& error)
		{
			const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
				CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
			if (FAILED(hr))
			{
				error = hr == CO_E_NOTINITIALIZED
					? L"COM 초기화가 필요합니다."
					: L"WIC 팩토리 생성 실패.";
				return false;
			}
			return true;
		}

		// 디코더에서 첫 프레임을 RGBA8 로 꺼낸다. 파일·메모리 경로가 공유한다.
		bool DecodeFirstFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
			bool srgb, const wchar_t* label, TextureData& out, std::wstring& error)
		{
			auto fail = [&](const wchar_t* reason)
			{
				error = std::wstring(reason) + L"\n" + (label ? label : L"");
				return false;
			};

			ComPtr<IWICBitmapFrameDecode> frame;
			if (FAILED(decoder->GetFrame(0, &frame)))
				return fail(L"텍스처 프레임 열기 실패.");

			UINT width = 0, height = 0;
			if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
				width > 16384 || height > 16384)
				return fail(L"텍스처 크기가 D3D12 의 2D 텍스처 범위를 벗어납니다.");

			const uint64_t bytes = uint64_t(width) * height * 4;
			if (bytes > (std::numeric_limits<UINT>::max)())
				return fail(L"텍스처 픽셀 버퍼가 너무 큽니다.");

			ComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(&converter)) ||
				FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
					WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
				return fail(L"텍스처 RGBA8 변환 실패.");

			TextureData data;
			data.path = label ? label : L"";
			data.width = width;
			data.height = height;
			data.srgb = srgb;
			data.pixels.resize(static_cast<size_t>(bytes));
			if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), data.pixels.data())))
				return fail(L"텍스처 픽셀 읽기 실패.");

			out = std::move(data);
			return true;
		}

	} // namespace

	bool LoadTextureImage(const wchar_t* path, bool srgb, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (!path || !*path)
		{
			error = L"텍스처 경로가 비어 있습니다.";
			return false;
		}

		ComPtr<IWICImagingFactory> factory;
		if (!CreateFactory(factory, error)) return false;

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"텍스처 이미지 열기 실패.\n") + path;
			return false;
		}

		return DecodeFirstFrame(factory.Get(), decoder.Get(), srgb, path, out, error);
	}

	bool LoadTextureImageFromMemory(const uint8_t* bytes, size_t size, bool srgb,
		const wchar_t* label, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (!bytes || size == 0)
		{
			error = L"내장 텍스처 바이트가 비어 있습니다.";
			return false;
		}
		if (size > (std::numeric_limits<DWORD>::max)())
		{
			error = L"내장 텍스처가 너무 큽니다.";
			return false;
		}

		ComPtr<IWICImagingFactory> factory;
		if (!CreateFactory(factory, error)) return false;

		// ★ WIC 스트림은 포인터를 들고만 있다 — 디코딩이 끝날 때까지 bytes 가 살아 있어야 한다.
		//   픽셀을 복사해 나가는 DecodeFirstFrame 안에서 끝나므로 호출자 버퍼로 충분하다.
		ComPtr<IWICStream> stream;
		if (FAILED(factory->CreateStream(&stream)) ||
			FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size))))
		{
			error = L"내장 텍스처 스트림 생성 실패.";
			return false;
		}

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"내장 텍스처 형식을 알 수 없습니다(PNG·JPEG 만).\n")
				+ (label ? label : L"");
			return false;
		}

		return DecodeFirstFrame(factory.Get(), decoder.Get(), srgb, label, out, error);
	}

	// ── DDS (2026-10-28 지형 레이어) ──────────────────────────────
	//  파일 배치(Microsoft «DDS file layout»): "DDS " 4바이트 + DDS_HEADER 124바이트 + DDS_HEADER_DXT10 20바이트 + 데이터.
	//  무압축 형식의 행 간격은 «폭 × 픽셀 바이트» 그대로(빈틈 없음)이고, 밉은 0 단계부터 차례로 붙어 있다.
	bool LoadDdsTexture(const wchar_t* path, TextureData& out, std::wstring& error)
	{
		error.clear();
		const std::wstring label = path ? path : L"";
		auto fail = [&](const wchar_t* reason)
		{
			error = std::wstring(reason) + L"\n" + label;
			return false;
		};
		if (!path || !*path) return fail(L"DDS 경로가 비어 있습니다.");

		std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
		if (!file) return fail(L"DDS 파일을 열지 못했습니다.");
		const std::streamoff size = file.tellg();
		constexpr size_t kHeaderBytes = 4 + 124 + 20;
		if (size < std::streamoff(kHeaderBytes)) return fail(L"DDS 파일이 머리말보다 작습니다.");
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return fail(L"DDS 파일 읽기 실패.");

		auto u32 = [&](size_t offset)
		{
			uint32_t value = 0;
			std::memcpy(&value, bytes.data() + offset, sizeof(value));
			return value;
		};
		constexpr uint32_t kDdsMagic = 0x20534444;            // "DDS "
		constexpr uint32_t kDx10FourCc = 0x30315844;          // "DX10"
		constexpr uint32_t kDdsdMipMapCount = 0x00020000;
		constexpr uint32_t kDimensionTexture2D = 3;           // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		constexpr uint32_t kMiscTextureCube = 0x4;
		constexpr uint32_t kFormatRgba8 = 28, kFormatRgba8Srgb = 29, kFormatR8 = 61;   // DXGI_FORMAT 값

		if (u32(0) != kDdsMagic || u32(4) != 124) return fail(L"DDS 머리말이 아닙니다.");
		const uint32_t flags = u32(8);
		const uint32_t height = u32(12);
		const uint32_t width = u32(16);
		const uint32_t mipCount = (flags & kDdsdMipMapCount) ? (std::max)(u32(28), 1u) : 1u;
		if (u32(84) != kDx10FourCc) return fail(L"DX10 머리말이 없는 DDS 는 읽지 않습니다(texconv -dx10 으로 만든 것만).");
		const uint32_t dxgiFormat = u32(128);
		const uint32_t dimension = u32(132);
		const uint32_t miscFlag = u32(136);
		const uint32_t arraySize = u32(140);
		if (dimension != kDimensionTexture2D || (miscFlag & kMiscTextureCube) || arraySize != 1)
			return fail(L"2D 텍스처 한 장인 DDS 만 읽습니다(큐브 · 배열 · 3D 아님).");

		TextureData data;
		data.path = label;
		data.width = width;
		data.height = height;
		data.mipLevels = mipCount;
		data.arraySize = 1;
		uint32_t bytesPerPixel = 4;
		switch (dxgiFormat)
		{
		case kFormatRgba8Srgb: data.format = TextureFormat::Rgba8; data.srgb = true;  break;
		case kFormatRgba8:     data.format = TextureFormat::Rgba8; data.srgb = false; break;
		case kFormatR8:        data.format = TextureFormat::R8;    data.srgb = false; bytesPerPixel = 1; break;
		default: return fail(L"지원하지 않는 DDS 형식입니다(RGBA8 · RGBA8 sRGB · R8 만).");
		}
		if (width == 0 || height == 0 || width > 16384 || height > 16384 || mipCount > 15)
			return fail(L"DDS 크기 · 밉 수가 D3D12 의 2D 텍스처 범위를 벗어납니다.");

		uint64_t dataBytes = 0;
		for (uint32_t mip = 0; mip < mipCount; ++mip)
			dataBytes += uint64_t((std::max)(width >> mip, 1u)) * (std::max)(height >> mip, 1u) * bytesPerPixel;
		if (bytes.size() - kHeaderBytes < dataBytes)
			return fail(L"DDS 데이터가 머리말이 말한 크기보다 짧습니다.");
		data.pixels.assign(bytes.begin() + kHeaderBytes, bytes.begin() + kHeaderBytes + static_cast<size_t>(dataBytes));
		out = std::move(data);
		return true;
	}

	bool CombineTextureArray(std::vector<TextureData>& slices, TextureData& out, std::wstring& error)
	{
		error.clear();
		if (slices.empty())
		{
			error = L"배열로 묶을 텍스처가 없습니다.";
			return false;
		}
		const TextureData& first = slices.front();
		size_t total = 0;
		for (const TextureData& slice : slices)
		{
			if (slice.width != first.width || slice.height != first.height || slice.format != first.format ||
				slice.srgb != first.srgb || slice.mipLevels != first.mipLevels || slice.arraySize != 1 ||
				slice.pixels.size() != first.pixels.size())
			{
				error = L"배열로 묶을 텍스처의 크기 · 형식 · 밉 수가 다릅니다: " + slice.path;
				return false;
			}
			total += slice.pixels.size();
		}
		TextureData data;
		data.path = first.path;
		data.width = first.width;
		data.height = first.height;
		data.srgb = first.srgb;
		data.format = first.format;
		data.mipLevels = first.mipLevels;
		data.arraySize = static_cast<uint32_t>(slices.size());
		data.pixels.reserve(total);
		for (TextureData& slice : slices)
		{
			data.pixels.insert(data.pixels.end(), slice.pixels.begin(), slice.pixels.end());
			std::vector<uint8_t>().swap(slice.pixels);   // 픽셀은 옮겼다 — 바로 놓는다(2048² × 4 장이면 수십 MB)
		}
		out = std::move(data);
		return true;
	}
}
