#include "Shared/Resource/HeightmapLoader.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace Shared {

	bool LoadHeightmapPng(const wchar_t* path, HeightmapData& out, std::wstring& error)
	{
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
			error = L"파일 열기 실패";
			return false;
		}

		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame)))
		{
			error = L"프레임 읽기 실패";
			return false;
		}

		// 원본이 8비트든 RGB든 16비트 그레이로 변환해서 받는다.
		// (8비트 원본이면 계단이 생기지만 로드 자체는 성공한다)
		ComPtr<IWICFormatConverter> conv;
		if (FAILED(factory->CreateFormatConverter(&conv)) ||
			FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat16bppGray,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			error = L"16비트 그레이 변환 실패";
			return false;
		}

		UINT w = 0, h = 0;
		conv->GetSize(&w, &h);
		if (w == 0 || h == 0)
		{
			error = L"크기 0";
			return false;
		}
		if (w != h)
		{
			error = L"정사각형 아님";
			return false;
		}

		out.size = w;
		out.samples.resize(size_t(w) * h);
		const UINT stride = w * sizeof(uint16_t);
		const UINT bytes = UINT(out.samples.size() * sizeof(uint16_t));
		if (FAILED(conv->CopyPixels(nullptr, stride, bytes,
			reinterpret_cast<BYTE*>(out.samples.data()))))
		{
			error = L"픽셀 복사 실패";
			return false;
		}

		// mean 은 로드 시 한 번만 구해 캐싱한다 (샘플마다 재계산 금지)
		double sum = 0.0;
		for (uint16_t s : out.samples) sum += s;
		out.mean = float(sum / double(out.samples.size()) / 65535.0);

		return true;
	}
}
