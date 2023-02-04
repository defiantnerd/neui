#include "widgetfactory.h"
#include "seat/seat.h"

namespace neui
{

	ISeat& seat()
	{
		return Seat::Instance();
	}
	// factory
	// std::shared_ptr<IPlatformView> createWidget(uint32_t type);
}