#include "TextureLoader.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

	// 2x2 박스 필터로 한 단계 줄인다. 홀수 크기의 마지막 줄·열은 가장자리를 한 번 더 쓴다.
	void Downsample(const uint8_t* src, uint32_t srcW, uint32_t srcH,
		uint8_t* dst, uint32_t dstW, uint32_t dstH)
	{
		for (uint32_t y = 0; y < dstH; ++y)
		{
			const uint32_t y0 = (y * 2 < srcH) ? y * 2 : srcH - 1;
			const uint32_t y1 = (y * 2 + 1 < srcH) ? y * 2 + 1 : y0;
			for (uint32_t x = 0; x < dstW; ++x)
			{
				const uint32_t x0 = (x * 2 < srcW) ? x * 2 : srcW - 1;
				const uint32_t x1 = (x * 2 + 1 < srcW) ? x * 2 + 1 : x0;

				const uint8_t* a = src + (size_t(y0) * srcW + x0) * 4;
				const uint8_t* b = src + (size_t(y0) * srcW + x1) * 4;
				const uint8_t* c = src + (size_t(y1) * srcW + x0) * 4;
				const uint8_t* d = src + (size_t(y1) * srcW + x1) * 4;
				uint8_t* o = dst + (size_t(y) * dstW + x) * 4;
				for (int ch = 0; ch < 4; ++ch)
					o[ch] = uint8_t((unsigned(a[ch]) + b[ch] + c[ch] + d[ch] + 2) / 4);
			}
		}
	}

}

namespace swc {

	bool LoadTextureFile(const wchar_t* path, TextureData& out, std::wstring& error)
	{
		out = TextureData{};

		ComPtr<IWICImagingFactory> factory;
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (FAILED(hr))
		{
			error = (hr == CO_E_NOTINITIALIZED)
				? L"COM 미초기화 (CoInitializeEx 필요)"
				: L"WIC 팩토리 생성 실패";
			return false;
		}

		ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(path, nullptr,
			GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			error = std::wstring(L"파일 열기 실패: ") + path;
			return false;
		}

		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame)))
		{
			error = L"프레임 읽기 실패";
			return false;
		}

		// RGB 든 RGBA 든 미리 곱한 RGBA8 로 받는다 (TextureData.h 주석).
		ComPtr<IWICFormatConverter> conv;
		if (FAILED(factory->CreateFormatConverter(&conv)) ||
			FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			error = L"RGBA8 변환 실패";
			return false;
		}

		UINT w = 0, h = 0;
		conv->GetSize(&w, &h);
		if (w == 0 || h == 0)
		{
			error = L"크기 0";
			return false;
		}

		// 밉 배치를 먼저 정하고 한 번에 할당한다.
		uint32_t mipW = w, mipH = h;
		size_t offset = 0;
		for (;;)
		{
			out.mips.push_back(TextureData::Mip{ mipW, mipH, offset });
			offset += size_t(mipW) * mipH * 4;
			if (mipW == 1 && mipH == 1) break;
			mipW = (mipW > 1) ? mipW / 2 : 1;
			mipH = (mipH > 1) ? mipH / 2 : 1;
		}
		out.pixels.resize(offset);
		out.width = w;
		out.height = h;

		const UINT stride = w * 4;
		if (FAILED(conv->CopyPixels(nullptr, stride, stride * h, out.pixels.data())))
		{
			out = TextureData{};
			error = L"픽셀 복사 실패";
			return false;
		}

		for (size_t i = 1; i < out.mips.size(); ++i)
		{
			const TextureData::Mip& src = out.mips[i - 1];
			const TextureData::Mip& dst = out.mips[i];
			Downsample(out.pixels.data() + src.offset, src.width, src.height,
				out.pixels.data() + dst.offset, dst.width, dst.height);
		}
		return true;
	}

} // namespace swc
