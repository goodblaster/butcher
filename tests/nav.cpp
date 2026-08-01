/**
 * @file nav.cpp
 *
 * Gate for keyboard navigation out of a text field.
 *
 * From a bug report: in the name field, pressing Down slid the caret to the
 * end of the name instead of moving to the next field. FTXUI's Input handles
 * ArrowUp and ArrowDown itself, so a vertical container never sees them and
 * focus is trapped.
 *
 * These tests drive real components with real events, so the first case also
 * demonstrates the trap -- if FTXUI ever stops consuming arrows, that test
 * fails and the wrapper can be removed.
 */
#include "../tui/nav.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"

#include <cstdio>
#include <string>

using namespace ftxui;

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
	if (cond) {
		g_pass++;
		printf("  ok    %s\n", what);
	} else {
		g_fail++;
		printf("  FAIL  %s\n", what);
	}
}

static void section(const char *name)
{
	printf("\n%s\n", name);
}

/** Which child of @p container currently holds focus. */
static int active_index(Component container)
{
	for (int i = 0; i < (int)container->ChildCount(); i++)
		if (container->ChildAt(i)->ActiveChild() != nullptr
		    || container->ActiveChild() == container->ChildAt(i))
			if (container->ActiveChild() == container->ChildAt(i))
				return i;
	return -1;
}

int main(void)
{
	printf("butcher -- terminal navigation\n");

	/* ---------------------------------------------------------------- */
	section("1. a bare Input traps vertical navigation");

	{
		std::string name = "rogue";
		int cursor = 0;
		InputOption opt = InputOption::Default();
		opt.multiline = false;
		opt.cursor_position = &cursor;

		auto input = Input(&name, "name", opt);
		bool flag = false;
		auto other = Checkbox("other", &flag);
		auto box = Container::Vertical({ input, other });

		box->SetActiveChild(input.get());
		cursor = 0;

		bool consumed = box->OnEvent(Event::ArrowDown);
		ok(consumed, "Input consumes ArrowDown rather than letting it navigate");
		ok(active_index(box) == 0, "  ...so focus never leaves the field");
		ok(cursor == (int)name.size(),
		    "  ...and the caret slid to the end of the text instead");
	}

	/* ---------------------------------------------------------------- */
	section("2. NavEscape lets the container navigate");

	{
		std::string name = "rogue";
		int cursor = 0;
		InputOption opt = InputOption::Default();
		opt.multiline = false;
		opt.cursor_position = &cursor;

		auto input = Input(&name, "name", opt);
		auto field = NavEscape(input);
		bool flag = false;
		auto other = Checkbox("other", &flag);
		auto box = Container::Vertical({ field, other });

		box->SetActiveChild(field.get());
		cursor = 0;

		box->OnEvent(Event::ArrowDown);
		ok(active_index(box) == 1, "ArrowDown moves to the next field");
		ok(cursor == 0, "  ...without disturbing the caret");

		box->OnEvent(Event::ArrowUp);
		ok(active_index(box) == 0, "ArrowUp comes back");
	}

	/* ---------------------------------------------------------------- */
	section("3. and still forwards everything else");

	{
		std::string name = "rogue";
		int cursor = 0;
		InputOption opt = InputOption::Default();
		opt.multiline = false;
		opt.cursor_position = &cursor;

		auto input = Input(&name, "name", opt);
		auto field = NavEscape(input);
		auto box = Container::Vertical({ field });
		box->SetActiveChild(field.get());

		cursor = (int)name.size();
		box->OnEvent(Event::Character('X'));
		ok(name == "rogueX", "typing still reaches the field");

		box->OnEvent(Event::Backspace);
		ok(name == "rogue", "backspace still reaches the field");

		cursor = 0;
		box->OnEvent(Event::ArrowRight);
		ok(cursor == 1, "ArrowRight still moves the caret");

		box->OnEvent(Event::ArrowLeft);
		ok(cursor == 0, "ArrowLeft still moves the caret");
	}

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
