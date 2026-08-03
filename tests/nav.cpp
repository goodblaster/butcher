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
#include <vector>

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

	/* ---------------------------------------------------------------- */
	section("4. switching tabs leaves focus somewhere usable");

	/*
	 * From a bug report: after tabbing between panes, Attributes was showing
	 * but arrows changed nothing. A pane with no focusable content -- the
	 * read-only Inventory -- made the whole tab body unfocusable, so an arrow
	 * press there moved focus up to the tab bar and the panes were left with
	 * nothing focused.
	 */
	{
		int va = 10, vb = 20;
		static int cap = 100;
		auto sa = Slider(SliderOption<int> { &va, 0, &cap, 1 });
		auto sb = Slider(SliderOption<int> { &vb, 0, &cap, 1 });
		auto attrs = Container::Vertical({ sa, sb });
		auto attrs_pane = Renderer(attrs, [] { return text("attrs"); });

		int vc = 5;
		auto sc = Slider(SliderOption<int> { &vc, 0, &cap, 1 });
		auto spells = Container::Vertical({ sc });
		auto spells_pane = Renderer(spells, [] { return text("spells"); });

		/* Focusable, as the real one now is. */
		auto inv_pane = Renderer([](bool) { return text("inv"); });

		int tab_index = 0, tab_cursor = 0;
		std::vector<std::string> names { "Attributes", "Spells", "Inventory" };
		MenuOption mo = MenuOption::Toggle();
		mo.entries = &names;
		mo.selected = &tab_index;
		mo.focused_entry = &tab_cursor;
		auto tab_bar = Menu(mo);
		auto tab_body = Container::Tab({ attrs_pane, spells_pane, inv_pane }, &tab_index);
		auto sheet = Container::Vertical({ tab_bar, tab_body });

		ok(tab_body->Focusable(), "the tab body is focusable on Attributes");
		tab_index = 2;
		ok(tab_body->Focusable(),
		    "  ...and still is on Inventory, which holds no controls");

		/* The switch itself, as the app performs it. */
		auto switch_to = [&](int want) {
			bool in_pane = tab_body->Focused();
			tab_index = want;
			tab_cursor = want;
			sheet->SetActiveChild(in_pane ? tab_body.get() : tab_bar.get());
		};

		tab_index = 0;
		tab_cursor = 0;
		sheet->SetActiveChild(tab_bar.get());
		sheet->OnEvent(Event::ArrowDown); /* into the pane */
		ok(sa->Focused(), "Down from the tab bar enters Attributes");

		switch_to(1);
		ok(sc->Focused(), "tabbing to Spells carries focus into it");
		switch_to(2);
		switch_to(0);
		ok(sa->Focused() || sb->Focused(),
		    "tabbing back through Inventory to Attributes leaves a field focused");

		int before = va + vb;
		sheet->OnEvent(Event::ArrowRight);
		ok(va + vb != before, "  ...and arrows change it");

		/* Starting on the tab bar, tabbing must not drag focus into a pane --
		 * the sheet opens there so Right switches panes immediately. */
		sheet->SetActiveChild(tab_bar.get());
		switch_to(1);
		ok(!sc->Focused() && tab_bar->Focused(),
		    "tabbing from the bar keeps focus on the bar");
	}

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
