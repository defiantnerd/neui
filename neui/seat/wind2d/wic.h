#pragma once

#include "base.h"

#include <wincodec.h>
#include "../resource.h"

namespace neui
{
	using namespace win;

	class WIC
	{
	public:
		WIC(IWICImagingFactory2& factory, ID2D1DeviceContext1& deviceContext)
			: wicFactory(factory)
			, deviceContext(deviceContext)
		{}
		D2D1BitmapRef createD2D1BitmapFromBuffer(const uint8_t* buffer, uint32_t bufferSize) const;
	protected:
		IWICImagingFactory2& wicFactory;
		ID2D1DeviceContext1& deviceContext;
	};
}
