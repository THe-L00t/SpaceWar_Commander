#include "TextureLoader.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl.h>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace swc {

	namespace {

		bool DecodeTexture(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
			const wchar_t* path, bool srgb, TextureData& out, std::wstring& error)
		{
			auto fail = [&](const wchar_t* reason)
			{
				error = std::wstring(reason) + L"\n" + path;
				return false;
			};

			ComPtr<IWICBitmapFrameDecode> frame;
			if (FAILED(decoder->GetFrame(0, &frame)))
				return fail(L"텍스처 프레임 읽기 실패.");

			UINT width = 0, height = 0;
			if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
				width > 16384 || height > 16384)
				return fail(L"텍스처 크기가 D3D12의 2D 텍스처 범위를 벗어납니다.");

			const uint64_t bytes = uint64_t(width) * height * 4;
			if (bytes > (std::numeric_limits<UINT>::max)())
				return fail(L"텍스처 픽셀 버퍼가 너무 큽니다.");

			ComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(&converter)) ||
				FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
					WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
				return fail(L"텍스처 RGBA8 변환 실패.");

			TextureData data;
			data.path = path;
			data.width = width;
			data.height = height;
			data.srgb = srgb;
			data.pixels.resize(static_cast<size_t>(bytes));
			if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), data.pixels.data())))
				return fail(L"텍스처 픽셀 읽기 실패.");

			out = std::move(data);
			return true;
		}

		bool CreateFactory(ComPtr<IWICImagingFactory>& factory, std::wstring& error)
		{
			const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
				CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
			if (FAILED(hr))
			{
				error = hr == CO_E_NOTINITIALIZED ? L"COM 초기화가 필요합니다." : L"WIC 팩토리 생성 실패.";
				return false;
			}
			return true;
		}

	}

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
		return DecodeTexture(factory.Get(), decoder.Get(), path, srgb, out, error);
	}

	bool LoadTextureImageFromMemory(const void* bytes, size_t byteCount, const wchar_t* sourceName,
		bool srgb, TextureData& out, std::wstring& error)
	{
		error.clear();
		const wchar_t* label = sourceName ? sourceName : L"내장 텍스처";
		if (!bytes || byteCount == 0 || byteCount > (std::numeric_limits<DWORD>::max)())
		{
			error = std::wstring(L"내장 텍스처의 압축 데이터 크기가 유효하지 않습니다.\n") + label;
			return false;
		}

		ComPtr<IWICImagingFactory> factory;
		if (!CreateFactory(factory, error)) return false;
		ComPtr<IWICStream> stream;
		if (FAILED(factory->CreateStream(&stream)) ||
			FAILED(stream->InitializeFromMemory(
				static_cast<BYTE*>(const_cast<void*>(bytes)), static_cast<DWORD>(byteCount))))
		{
			error = std::wstring(L"내장 텍스처 스트림 생성 실패.\n") + label;
			return false;
		}

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"내장 텍스처 이미지 읽기 실패.\n") + label;
			return false;
		}
		// WIC가 참조하는 입력 버퍼가 살아 있는 동안 RGBA8 픽셀 복사를 마친다.
		return DecodeTexture(factory.Get(), decoder.Get(), label, srgb, out, error);
	}
}
