/**
 * @file nav.cpp
 *
 * See nav.h.
 */
#include "nav.h"

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"

using namespace ftxui;

namespace {

class NavEscapeBase : public ComponentBase {
public:
	explicit NavEscapeBase(Component child) { Add(child); }

	bool OnEvent(Event e) override
	{
		if (e == Event::ArrowUp || e == Event::ArrowDown)
			return false;
		return ComponentBase::OnEvent(e);
	}
};

} // namespace

Component NavEscape(Component child)
{
	return Make<NavEscapeBase>(child);
}
