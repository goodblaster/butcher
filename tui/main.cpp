/**
 * @file main.cpp
 *
 * The terminal interface.
 *
 * One of two front ends over the library in src/; src/butcher.cpp chooses
 * between them. It does not invoke the command-line side -- both link src/
 * directly. That is why the slider bounds can be hero_max_stat() and the
 * warning strip can be hero_check(): there is no serialization boundary to
 * keep in sync, and no schema to drift.
 *
 *   butcher [dir-or-save]         browse, or open one save
 *   butcher <save> --render       draw one frame and exit (no terminal needed)
 *   butcher <save> --tab N        select a pane before rendering
 *   butcher <save> --focus N      place the cursor before rendering
 */
#include "../src/charjson.h"
#include "../src/format.h"
#include "../src/saveutil.h"
#include "editor.h"
#include "nav.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include <sys/stat.h>

#include <string>
#include <vector>

using namespace ftxui;

namespace {

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

std::string Pad(const std::string &s, size_t n)
{
	return s.size() >= n ? s : s + std::string(n - s.size(), ' ');
}

/** The last path component -- status lines have no room for a full path. */
std::string Leaf(const std::string &path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Commas(long long v)
{
	std::string d = std::to_string(v < 0 ? -v : v);
	std::string out;
	for (size_t i = 0; i < d.size(); i++) {
		if (i > 0 && (d.size() - i) % 3 == 0)
			out += ',';
		out += d[i];
	}
	return (v < 0 ? "-" : "") + out;
}

/* ------------------------------------------------------------------ */
/* Editor state                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Rendering helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * Mark the row the cursor is on.
 *
 * A slider recolours itself when focused, but an empty gauge -- any value at
 * its minimum -- has nothing to recolour, so at level 0 there was no way to
 * tell which row you were on. The marker and emphasis do not depend on the
 * value. `focus` additionally tells an enclosing frame to scroll here.
 */
Element MarkRow(bool focused, Element row, bool owns_cursor = false)
{
	Element marked = hbox({
	    text(focused ? "\u25b8 " : "  ") | color(Color::Cyan),
	    row | flex,
	});
	if (!focused)
		return marked;
	/*
	 * No background tint: FTXUI draws an unfocused gauge in GrayDark, so a
	 * GrayDark bar rendered the level blocks invisible on the very row the
	 * cursor was on. The marker and bold carry the highlight instead.
	 *
	 * `focus` is withheld from widgets that position the terminal cursor
	 * themselves -- Input uses focusCursorBarBlinking, and a competing focus
	 * on the row parked the caret at the row start instead of in the text.
	 */
	if (owns_cursor)
		return marked | bold;
	return marked | bold | focus;
}

/** One labelled slider row with its range shown alongside. */
Element StatRow(const std::string &label, int value, int cap, Component slider)
{
	Color tint = (cap > 0 && value >= cap) ? Color::Yellow : Color::Green;
	return MarkRow(slider->Focused(),
	    hbox({
	        text(Pad(label, 11)),
	        text(Pad(std::to_string(value), 5)) | color(tint),
	        slider->Render() | flex,
	        text("  0-" + std::to_string(cap)) | dim,
	    }));
}

Element Diagnostics(const PkPlayerStruct &h, HeroFlavor f, bool level_raised,
    int *errors, int *warnings)
{
	DiagList dl;
	dl_init(&dl);
	hero_check(&h, f, &dl);

	std::vector<Element> notes;
	int e = 0, w = 0;
	for (int i = 0; i < dl.n; i++) {
		bool is_error = dl.items[i].level == DIAG_ERROR;
		std::string where = dl.items[i].where;
		/* When the user deliberately raised experience, "the game will level
		 * you up" is the intended effect, shown better on the level row. */
		if (!is_error && where == "level" && level_raised)
			continue;
		(is_error ? e : w)++;
		notes.push_back(hbox({
		                    text(is_error ? "  ✗ " : "  ! "),
		                    paragraph((where.empty() ? "" : where + ": ") + dl.items[i].msg)
		                        | flex,
		                })
		    | color(is_error ? Color::Red : Color::Yellow));
	}
	dl_free(&dl);

	*errors = e;
	*warnings = w;
	if (notes.empty())
		notes.push_back(text("  ✓ valid") | color(Color::Green));
	return vbox(notes);
}

/** The 10x4 inventory, drawn the way the game lays it out. */
Element InventoryGrid(const PkPlayerStruct &h)
{
	std::vector<Element> rows;
	for (int r = 0; r < 4; r++) {
		std::vector<Element> cells;
		for (int c = 0; c < 10; c++) {
			int v = h.InvGrid[r * 10 + c];
			if (v == 0) {
				cells.push_back(text(" · ") | dim);
			} else {
				int idx = (v > 0 ? v : -v) - 1;
				bool gold = idx >= 0 && idx < NUM_INV_GRID_ELEM
				    && h.InvList[idx].idx == IDI_GOLD;
				std::string label = v > 0 ? std::to_string(idx + 1) : "·";
				cells.push_back(text(Pad(" " + label, 3))
				    | color(gold ? Color::Yellow : Color::Cyan));
			}
		}
		rows.push_back(hbox(cells));
	}
	return vbox(rows);
}

} // namespace

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

/* Entry point when no subcommand is given; see butcher.cpp for dispatch. */
int tui_main(int argc, char **argv)
{
	std::vector<SaveEntry> saves(SAVE_MAX_SLOTS);
	char used_dir[SAVE_PATH_MAX] = { 0 };
	int nsaves = 0;
	int want_render = 0;
	int want_saves = 0;
	const char *arg = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--render") == 0)
			want_render = 1;
		else if (strcmp(argv[i], "--saves") == 0)
			want_saves = 1;
		else if (strncmp(argv[i], "--", 2) == 0)
			continue; /* --tab, --focus and the flavour flags */
		else if (arg == nullptr)
			arg = argv[i];
	}

	/* A named save opens directly; anything that scans a directory picks. */
	int named_one = 0;

	if (arg != nullptr) {
		struct stat st;
		if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
			nsaves = save_scan_dir(arg, saves.data(), SAVE_MAX_SLOTS);
			snprintf(used_dir, sizeof(used_dir), "%s", arg);
		} else {
			named_one = 1;
			PkPlayerStruct h;
			char err[MPQ_ERR_LEN];
			if (!save_read_hero(arg, &h, nullptr, err)) {
				fprintf(stderr, "butcher: %s\n", err);
				return 1;
			}
			SaveEntry &e = saves[0];
			snprintf(e.path, sizeof(e.path), "%s", arg);
			memcpy(e.name, h.pName, PLR_NAME_LEN);
			e.name[PLR_NAME_LEN] = '\0';
			e.slot = 0;
			e.flavor = save_flavor_of(arg);
			e.in_progress = save_has_game(arg);
			e.hero = h;
			nsaves = 1;
		}
	} else if (want_saves) {
		nsaves = save_scan_default(saves.data(), SAVE_MAX_SLOTS, used_dir);
		if (nsaves == 0) {
			fprintf(stderr, "butcher: no saves found in the usual places%s%s\n",
			    used_dir[0] ? ", including " : "", used_dir);
			return 1;
		}
	} else {
		/*
		 * Bare invocation. The current directory is the only context this tool
		 * is entitled to assume; reaching into the game's save folder because
		 * it happens to exist would mean a file editor picking its own target,
		 * with no relation to where the user is standing.
		 */
		nsaves = save_scan_dir(".", saves.data(), SAVE_MAX_SLOTS);
		if (nsaves == 0) {
			char elsewhere[SAVE_PATH_MAX] = { 0 };
			SaveEntry probe[SAVE_MAX_SLOTS];
			int found = save_scan_default(probe, SAVE_MAX_SLOTS, elsewhere);

			printf("butcher -- Diablo and Hellfire character editor\n\n");
			if (found > 0) {
				printf("No saves in this directory. %d found in your game's "
				       "save folder:\n\n",
				    found);
				printf("    %s\n\n", elsewhere);
				printf("    butcher --saves        open them\n");
			} else {
				printf("No saves in this directory, and none in the usual "
				       "places.\n\n");
			}
			printf("    butcher <save>         open one character\n");
			printf("    butcher <directory>    browse a directory\n");
			printf("    butcher --help         every command\n");
			return 0;
		}
		snprintf(used_dir, sizeof(used_dir), ".");
	}

	if (nsaves == 0) {
		fprintf(stderr, "butcher: no readable saves found%s%s\n",
		    used_dir[0] ? " in " : "", used_dir);
		fprintf(stderr, "pass a save file or a directory to look in.\n");
		return 1;
	}
	saves.resize(nsaves);

	/* ---- shared state ---- */
	Editor ed;
	/*
	 * 0 picker, 1 sheet. Only naming a save file opens it directly. Pointing
	 * at a directory means "show me what is here" -- jumping into whichever
	 * character happened to sort first is the tool choosing for you, and with
	 * one save in the folder it is not even obvious that it chose.
	 */
	int screen_index = named_one ? 1 : 0;
	int picked = 0;
	int tab_index = 0;
	bool name_was_focused = false;
	bool backup = true;
	bool confirming = false;
	std::string status;
	Color status_color = Color::GrayLight;

	/*
	 * Load unconditionally, even when the picker is shown first. The
	 * components below bind to addresses inside `ed`, so it must hold a real
	 * character before any of them exist.
	 */
	ed.Load(saves[0]);

	/* ---- picker ---- */
	std::vector<std::string> picker_items;
	for (const auto &s : saves) {
		/* The "!" marks a save whose character the game loads from "game"
		 * rather than from the "hero" this edits. */
		picker_items.push_back(std::string(s.in_progress ? "! " : "  ")
		    + Pad(s.name, 18) + Pad(hero_flavor_name(s.flavor), 10)
		    + Pad(hero_class_name(s.flavor, s.hero.pClass), 11) + "lvl "
		    + Pad(std::to_string(s.hero.pLevel), 4) + Commas(s.hero.pGold) + " gold");
	}
	bool any_in_progress = false;
	for (const auto &s2 : saves)
		if (s2.in_progress)
			any_in_progress = true;

	auto picker_menu = Menu(&picker_items, &picked);
	auto picker = Renderer(picker_menu, [&] {
		return vbox({
		           text(" butcher ") | bold,
		           text(std::string(" ") + used_dir) | dim,
		           separator(),
		           picker_menu->Render() | frame | flex,
		           separator(),
		           any_in_progress
		               ? text(" ! has a game in progress; edits are applied to the "
		                      "saved game too ")
		                   | color(Color::Cyan)
		               : text(""),
		           text(" ↑↓ choose · enter open · q quit ") | dim,
		       })
		    | border;
	});

	/* ---- attributes pane ---- */
	std::vector<std::string> class_names;
	/* Rebuilt whenever a character is opened: Hellfire has six classes, Diablo
	 * three, and the list must match the save actually in front of you. */
	auto reload_classes = [&] {
		class_names.clear();
		for (int i = 0; i < hero_num_classes(ed.entry.flavor); i++)
			class_names.push_back(hero_class_name(ed.entry.flavor, i));
	};
	reload_classes();

	/*
	 * Single line, and sanitised as it is typed. The UI should not accept
	 * something validation will then reject: Return used to insert a newline,
	 * and hero_name_valid refuses control characters.
	 */
	int name_cursor = 0;
	InputOption name_opt = InputOption::Default();
	name_opt.multiline = false;
	name_opt.cursor_position = &name_cursor;
	name_opt.on_change = [&] {
		std::string clean = editor_sanitize_name(ed.name);
		if (clean != ed.name)
			ed.name = clean;
	};
	auto name_input = Input(&ed.name, "name", name_opt);
	/* Ctrl+U clears the field, so a name can be replaced rather than edited. */
	auto name_field = NavEscape(CatchEvent(name_input, [&](Event e) {
		if (e == Event::CtrlU) {
			ed.name.clear();
			name_cursor = 0;
			return true;
		}
		return false;
	}));
	auto class_pick = Radiobox(&class_names, &ed.cls);

	auto slider_str = Slider(SliderOption<int> { &ed.str, 0, &ed.cap_str, 1 });
	auto slider_mag = Slider(SliderOption<int> { &ed.mag, 0, &ed.cap_mag, 1 });
	auto slider_dex = Slider(SliderOption<int> { &ed.dex, 0, &ed.cap_dex, 1 });
	auto slider_vit = Slider(SliderOption<int> { &ed.vit, 0, &ed.cap_vit, 1 });
	int cap_stat_pts = 255, cap_level = hero_max_level();
	auto slider_pts = Slider(SliderOption<int> { &ed.statpts, 0, &cap_stat_pts, 1 });
	auto slider_lvl = Slider(SliderOption<int> { &ed.level_target, 1, &cap_level, 1 });
	auto slider_hp = Slider(SliderOption<int> { &ed.hp, 1, &ed.hp_max, 1 });
	auto slider_hpmax = Slider(SliderOption<int> { &ed.hp_max, 1, &ed.cap_life, 1 });
	auto slider_mana = Slider(SliderOption<int> { &ed.mana, 0, &ed.mana_max, 1 });
	auto slider_manamax = Slider(SliderOption<int> { &ed.mana_max, 0, &ed.cap_mana, 1 });
	auto slider_gold = Slider(SliderOption<int> { &ed.gold, 0, &ed.cap_gold, 100 });
	auto slider_dlvl = Slider(SliderOption<int> { &ed.dlvl, 0, &ed.cap_dlvl, 1 });

	auto attrs_controls = Container::Vertical({
	    name_field,
	    class_pick,
	    slider_str,
	    slider_mag,
	    slider_dex,
	    slider_vit,
	    slider_pts,
	    slider_lvl,
	    slider_hp,
	    slider_hpmax,
	    slider_mana,
	    slider_manamax,
	    slider_gold,
	    slider_dlvl,
	});

	auto attrs_pane = Renderer(attrs_controls, [&] {
		ed.RefreshCaps();
		PkPlayerStruct h = ed.Compose();

		std::vector<Element> rows;
		{
			/*
			 * Sized to what a name can hold rather than flexed across the row,
			 * so the highlight is the field and not the whole line.
			 */
			bool on_name = name_input->Focused();
			if (on_name && !name_was_focused)
				name_cursor = (int)ed.name.size(); /* arrive at the end, not the left */
			name_was_focused = on_name;

			Element field = name_input->Render()
			    | size(WIDTH, EQUAL, PLR_NAME_LEN);
			rows.push_back(MarkRow(on_name,
			    hbox({
			        text(Pad("Name", 11)),
			        on_name ? (field | underlined | color(Color::White))
			                : (field | dim),
			        filler(),
			        text(on_name ? "  ^U clears" : "") | dim,
			    }),
			    /*owns_cursor=*/true));
		}
		rows.push_back(MarkRow(class_pick->Focused(),
		    hbox({
		        text(Pad("Class", 11)),
		        class_pick->Render() | flex,
		    })));
		rows.push_back(separator());
		rows.push_back(StatRow("Strength", ed.str, ed.cap_str, slider_str));
		rows.push_back(StatRow("Magic", ed.mag, ed.cap_mag, slider_mag));
		rows.push_back(StatRow("Dexterity", ed.dex, ed.cap_dex, slider_dex));
		rows.push_back(StatRow("Vitality", ed.vit, ed.cap_vit, slider_vit));
		rows.push_back(StatRow("Unspent", ed.statpts, cap_stat_pts, slider_pts));
		rows.push_back(separator());
		rows.push_back(StatRow("Level", ed.level_target, cap_level, slider_lvl));

		std::string exp_line = "  " + Pad("Experience", 11) + Commas(h.pExperience);
		if (h.pExperience != ed.original.pExperience)
			exp_line = "  " + Pad("Experience", 11) + Commas(ed.original.pExperience)
			    + " -> " + Commas(h.pExperience);
		rows.push_back(text(exp_line));

		/*
		 * The slider shows the level the experience is worth; the game shows
		 * the level it has actually awarded, and only catches up when
		 * AddPlrExperience next runs -- on a kill. Say so whenever the two
		 * disagree, not just while the slider is being moved. Hiding it after
		 * a save left the sheet reading "Level 20" for a character the game
		 * still calls level 3, which reads as the edit having failed.
		 */
		int gained = ed.level_target - ed.level_stored;
		if (gained > 0) {
			/* NextPlrLevel pays out per level: +5 stat points and a
			 * class-dependent life bump, on the next experience gained. */
			int hp_per = (ed.cls == PC_SORCERER ? 64 : 128) + 1;
			rows.push_back(text("   └ in game: level "
			                  + std::to_string(ed.level_stored) + " until your next kill, "
			                  + "then " + std::to_string(ed.level_target) + " (+"
			                  + std::to_string(gained * 5) + " pts, +"
			                  + std::to_string((gained * hp_per) >> HERO_FIXED_SHIFT)
			                  + " life)")
			    | color(Color::Cyan));
		} else if (gained < 0) {
			rows.push_back(text("   └ in game still level "
			                  + std::to_string(ed.level_stored)
			                  + "; the game never takes levels back")
			    | color(Color::Yellow));
		}

		rows.push_back(separator());
		rows.push_back(StatRow("Life", ed.hp, ed.hp_max, slider_hp));
		rows.push_back(StatRow("Life max", ed.hp_max, ed.cap_life, slider_hpmax));
		rows.push_back(StatRow("Mana", ed.mana, ed.mana_max, slider_mana));
		rows.push_back(StatRow("Mana max", ed.mana_max, ed.cap_mana, slider_manamax));
		rows.push_back(separator());
		rows.push_back(MarkRow(slider_gold->Focused(),
		    hbox({
		        text(Pad("Gold", 11)),
		        text(Pad(Commas(ed.gold), 10)) | color(Color::Yellow),
		        slider_gold->Render() | flex,
		        text("  0-" + Commas(ed.cap_gold)) | dim,
		    })));
		int nstacks = (ed.gold + GOLD_MAX_LIMIT - 1) / GOLD_MAX_LIMIT;
		rows.push_back(text("   └ " + std::to_string(nstacks)
		                  + (nstacks == 1 ? " stack of at most " : " stacks of at most ")
		                  + Commas(GOLD_MAX_LIMIT)
		                  + ", " + std::to_string(hero_free_inv_cells(&h))
		                  + " cells free")
		    | dim);
		rows.push_back(StatRow("Dungeon", ed.dlvl, ed.cap_dlvl, slider_dlvl));

		return vbox(rows);
	});

	/* ---- spells pane ---- */
	/*
	 * Every id either game names, so the component set is fixed for the life
	 * of the program. Rows the current flavour does not define are wrapped in
	 * Maybe so they are skipped for focus as well as hidden -- hiding them at
	 * render time alone left them reachable, which is how a Hellfire spell got
	 * toggled into a Diablo save and made the character unloadable.
	 */
	std::vector<int> spell_ids;
	for (int s = 1; s < hero_spell_persisted(); s++)
		if (hero_spell_name(FLAVOR_DIABLO, s) != nullptr
		    || hero_spell_name(FLAVOR_HELLFIRE, s) != nullptr)
			spell_ids.push_back(s);

	auto spell_controls = Container::Vertical({});
	std::vector<Component> spell_sliders;
	static int cap_spell = 15;
	for (size_t i = 0; i < spell_ids.size(); i++) {
		int s = spell_ids[i];
		/*
		 * Only the slider is focusable. A checkbox beside it in a horizontal
		 * container meant Down landed on the checkbox and Right merely moved
		 * to the slider, so two presses were needed before a level changed --
		 * and a slider consumes Left/Right, so putting it first would trap
		 * focus in the row. The book bit is toggled with Space instead, and
		 * the checkbox is drawn as a glyph rather than a widget.
		 */
		SliderOption<int> opt { &ed.spell_lvl[s], 0, &cap_spell, 1 };
		opt.color_active = Color::Cyan;
		opt.color_inactive = Color::GrayLight; /* legible when not focused */
		auto row = CatchEvent(Slider(opt), [&, s](Event e) {
			if (e == Event::Character(' ') || e == Event::Return) {
				/*
				 * Spells with no book are granted by the class; the game masks
				 * them straight back out of _pMemSpells. Say so rather than
				 * doing nothing -- a key that silently no-ops on some rows
				 * reads as a broken key, not as a rule.
				 */
				if (!hero_spell_has_book(ed.entry.flavor, s)) {
					const char *nm = hero_spell_name(ed.entry.flavor, s);
					status = std::string(nm ? nm : "that spell")
					    + " has no book -- the game grants it as a skill, so the "
					      "book bit would not survive loading";
					status_color = Color::Yellow;
					return true;
				}
				ed.spell_known[s] = !ed.spell_known[s];
				status.clear();
				return true;
			}
			return false;
		});
		spell_sliders.push_back(row);
		spell_controls->Add(
		    Maybe(row, [&ed, s] { return hero_spell_exists(ed.entry.flavor, s) != 0; }));
	}

	auto spells_pane = Renderer(spell_controls, [&] {
		std::vector<Element> rows;
		rows.push_back(hbox({
		                   text("   "),                /* marker + glyph */
		                   text(" " + Pad("spell", 18)),
		                   text(Pad("level", 4)),
		               })
		    | dim);
		rows.push_back(text("   space toggles the spell book; \u00b7 marks a spell "
		                    "that has none") 
		    | dim);
		rows.push_back(separator());
		for (size_t i = 0; i < spell_ids.size(); i++) {
			int s = spell_ids[i];
			if (!hero_spell_exists(ed.entry.flavor, s))
				continue; /* not a spell in this game */
			const char *nm = hero_spell_name(ed.entry.flavor, s);
			bool book = hero_spell_has_book(ed.entry.flavor, s) != 0;
			bool active = ed.spell_known[s] || ed.spell_lvl[s] > 0;
			bool here = spell_sliders[i]->Focused();
			/* A class skill has no book, so it gets no checkbox to toggle. */
			Element mark = book ? text(ed.spell_known[s] ? "▣" : "☐")
			        | color(ed.spell_known[s] ? Color::Green : Color::GrayDark)
			                    : text("·") | color(Color::GrayDark);
			Element row = hbox({
			                  mark,
			                  text(" " + Pad(nm ? nm : "?", 18)),
			                  text(Pad(std::to_string(ed.spell_lvl[s]), 4)),
			                  spell_sliders[i]->Render() | flex,
			              })
			    | (active || here ? nothing : dim);
			rows.push_back(MarkRow(here, row));
		}
		return vbox(rows) | vscroll_indicator | frame;
	});

	/* ---- inventory pane (read only) ---- */
	/*
	 * Focusable even though it holds no controls. A Renderer with no children
	 * is not, which made the whole tab body unfocusable while Inventory was
	 * selected -- so an arrow press there moved focus up to the tab bar and
	 * left the panes with nothing focused.
	 */
	auto inventory_pane = Renderer([&](bool) {
		PkPlayerStruct h = ed.Compose();
		std::vector<Element> left;
		left.push_back(text(" Equipped") | bold);
		static const char *slots[NUM_INVLOC] = { "head", "left ring", "right ring",
			"amulet", "left hand", "right hand", "chest" };
		for (int i = 0; i < NUM_INVLOC; i++) {
			const PkItemStruct &it = h.InvBody[i];
			std::string what = "-";
			if (it.idx != 0xFFFF) {
				const char *nm = format_item_name(ed.entry.flavor, it.idx);
				what = nm ? nm : "unknown";
			}
			left.push_back(text("  " + Pad(slots[i], 12) + what));
		}

		std::vector<Element> right;
		right.push_back(text(" Inventory") | bold);
		right.push_back(InventoryGrid(h));
		right.push_back(text(""));
		int shown = 0;
		for (int i = 0; i < h._pNumInv && i < NUM_INV_GRID_ELEM && shown < 12; i++) {
			const PkItemStruct &it = h.InvList[i];
			if (it.idx == 0xFFFF)
				continue;
			std::string what = it.idx == IDI_GOLD
			    ? Commas(it.wValue) + " gold"
			    : std::string(format_item_name(ed.entry.flavor, it.idx)
			              ? format_item_name(ed.entry.flavor, it.idx)
			              : "unknown");
			right.push_back(text("  " + Pad(std::to_string(i + 1) + ".", 4) + what));
			shown++;
		}

		return vbox({
		    hbox({ vbox(left) | flex, separator(), vbox(right) | flex }),
		    separator(),
		    text("  items are generated from a seed and cannot be edited; shown "
		         "for reference")
		        | dim,
		});
	});

	/* ---- sheet ---- */
	std::vector<std::string> tab_names = { "Attributes", "Spells", "Inventory" };
	/*
	 * A Menu tracks two positions: which entry is selected, and where its own
	 * cursor sits. Toggle(&names, &index) only exposes the first, so changing
	 * the index from a key handler moved the selection while leaving the
	 * cursor behind -- the bar then showed one tab bold and a different one
	 * reversed. Binding both means they cannot disagree.
	 */
	int tab_cursor = 0;
	MenuOption tab_opt = MenuOption::Toggle();
	tab_opt.entries = &tab_names;
	tab_opt.selected = &tab_index;
	tab_opt.focused_entry = &tab_cursor;
	auto tab_bar = Menu(tab_opt);
	auto tab_body = Container::Tab({ attrs_pane, spells_pane, inventory_pane }, &tab_index);
	auto sheet_container = Container::Vertical({ tab_bar, tab_body });

	auto sheet = Renderer(sheet_container, [&] {
		PkPlayerStruct h = ed.Compose();
		int errors = 0, warnings = 0;
		bool raised = ed.level_target > ed.level_opened;
		Element diag = Diagnostics(h, ed.entry.flavor, raised, &errors, &warnings);
		bool dirty = memcmp(&h, &ed.original, sizeof(h)) != 0;

		Element body = vbox({
		    /*
		     * The in-progress marker rides on the header rather than taking a
		     * row of its own: a whole line costs more than it is worth, and on
		     * a short terminal it pushed Level and Experience out of view.
		     */
		    hbox({
		        text(" " + ed.name + " ") | bold,
		        filler(),
		        ed.entry.in_progress ? text("game in progress  ") | color(Color::Cyan)
		                             : text(""),
		        text(std::string(hero_flavor_name(ed.entry.flavor)) + " "
		            + hero_class_name(ed.entry.flavor, ed.cls) + " ")
		            | dim,
		    }),
		    text(std::string(" ") + ed.entry.path) | dim,
		    separator(),
		    tab_bar->Render(),
		    separator(),
		    tab_body->Render() | flex,
		    separator(),
		    diag,
		    separator(),
		    hbox({
		        text(dirty ? " modified" : " unchanged")
		            | color(dirty ? Color::Yellow : Color::GrayDark),
		        text(backup ? "  backup on" : "  backup off") | dim,
		        filler(),
		        text(status) | color(status_color),
		        text("  " + std::to_string(errors) + "E " + std::to_string(warnings)
		            + "W ")
		            | dim,
		    }),
		    text(std::string(" tab pane · ↑↓ field · ←→ adjust")
		        + (tab_index == 1 ? " · space toggle" : "")
		        + " · ^S save · ^F fix · ^R revert · ^B backup"
		        + (!named_one ? " · esc back" : "") + " · q quit ")
		        | dim,
		});

		if (!confirming)
			return body | border;

		/* Writing is the one irreversible step; make it deliberate. */
		Element ask = vbox({
		                  text(" Save changes? ") | bold,
		                  separator(),
		                  text(" " + std::string(ed.entry.path)),
		                  text(backup ? " a backup will be written first"
		                              : " NO backup will be written")
		                      | color(backup ? Color::Green : Color::Red),
		                  errors > 0
		                      ? text(" " + std::to_string(errors)
		                            + " errors -- refusing")
		                          | color(Color::Red)
		                      : text(" y confirm   n cancel") | dim,
		              })
		    | border | bgcolor(Color::Black);
		return dbox({ body | border, ask | center });
	});

	/* ---- events ---- */
	auto root_container = Container::Tab({ picker, sheet }, &screen_index);

	auto app = CatchEvent(root_container, [&](Event e) {
		if (confirming) {
			if (e == Event::Character('y')) {
				PkPlayerStruct h = ed.Compose();
				char err[HERO_ERR_LEN];
				if (!hero_validate(&h, ed.entry.flavor, err)) {
					status = std::string("refused: ") + err;
					status_color = Color::Red;
				} else {
					char werr[MPQ_ERR_LEN];
					char bak[SAVE_PATH_MAX] = { 0 };
					bool ok = true;
					if (backup)
						ok = save_backup_to(ed.entry.path, bak, sizeof(bak), werr) != 0;
					SaveGameSync sync = SAVE_GAME_ABSENT;
					if (ok)
						ok = save_commit_ex(ed.entry.path, &h, /*backup=*/0, &sync,
						         werr)
						    != 0;
					if (ok) {
						ed.original = h;
						ed.level_opened = ed.level_target;
						ed.level_stored = h.pLevel;
						ed.gold_opened = ed.gold;
						/* Name the backup: it is not always <save>.bak, and the
						 * user needs to know which file to restore from. */
						status = backup ? std::string("saved -- backup ") + Leaf(bak)
						                : std::string("saved");
						if (sync == SAVE_GAME_SYNCED)
							status += " (saved game updated too)";
						else if (sync == SAVE_GAME_SYNCED_NO_ITEMS) {
							status += " -- but the inventory was not carried into "
							          "the saved game, so the gold total will not "
							          "stick";
							status_color = Color::Yellow;
						}
						status_color = Color::Green;
					} else {
						status = std::string("failed: ") + werr;
						status_color = Color::Red;
					}
				}
				confirming = false;
				return true;
			}
			if (e == Event::Character('n') || e == Event::Escape) {
				confirming = false;
				return true;
			}
			return true; /* swallow everything else while asking */
		}

		if (screen_index == 0) {
			if (e == Event::Return) {
				ed.Load(saves[picked]);
				reload_classes();
				status.clear();
				tab_index = 0;
				/* Start on the tab bar: Right switches pane, Down enters it. */
				sheet_container->SetActiveChild(tab_bar.get());
				attrs_controls->SetActiveChild(name_field.get());
				screen_index = 1;
				return true;
			}
			if (e == Event::Character('q')) {
				ScreenInteractive::Active()->Exit();
				return true;
			}
			return false;
		}

		/* Tab cycles panes; the bar's cursor has to follow the selection. */
		if (e == Event::Tab || e == Event::TabReverse) {
			/*
			 * Where focus ends up has to be decided here, not left to whatever
			 * it happened to be. Changing the index alone could leave focus on
			 * a pane that is no longer shown, or nowhere at all -- and then
			 * arrows did nothing until you found your way back by hand.
			 */
			bool was_in_pane = tab_body->Focused();
			int n = (int)tab_names.size();
			tab_index = (tab_index + (e == Event::Tab ? 1 : n - 1)) % n;
			tab_cursor = tab_index;
			sheet_container->SetActiveChild(
			    was_in_pane ? tab_body.get() : tab_bar.get());
			return true;
		}

		/* Ctrl chords are only meaningful once a character is open. */
		if (e == Event::CtrlS) {
			confirming = true;
			return true;
		}
		if (e == Event::CtrlR) {
			ed.Load(ed.entry);
			status = "reverted";
			status_color = Color::GrayLight;
			return true;
		}
		if (e == Event::CtrlB) {
			backup = !backup;
			return true;
		}
		/*
		 * Repair. The sheet refuses to save an invalid character, so without
		 * this there is no way out of one from inside the UI. Nothing is
		 * written -- ^S still saves and ^R still throws it away.
		 */
		if (e == Event::CtrlF) {
			PkPlayerStruct h = ed.Compose();
			DiagList log;
			dl_init(&log);
			int n = hero_fix(&h, ed.entry.flavor, /*settle_warnings=*/1, &log);
			std::string first;
			if (log.n > 0)
				first = log.items[0].msg;
			dl_free(&log);

			if (n == 0) {
				status = "nothing to repair";
				status_color = Color::GrayLight;
			} else {
				ed.ApplyRepair(h);
				reload_classes();
				status = std::to_string(n) + (n == 1 ? " fix: " : " fixes, first: ")
				    + first + "  (^S save, ^R undo)";
				status_color = Color::Green;
			}
			return true;
		}
		/* Back to the picker, unless a single save was named on the command
		 * line and there is no picker to go back to. */
		if (e == Event::Escape && !named_one) {
			screen_index = 0;
			return true;
		}
		/* `q` must not quit while a text field has focus, or names cannot
		 * contain the letter. */
		if (e == Event::Character('q') && !name_input->Focused()) {
			ScreenInteractive::Active()->Exit();
			return true;
		}
		return false;
	});

	/* --tab N selects a pane before rendering, so each can be inspected. */
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], "--tab") == 0) {
			tab_index = atoi(argv[i + 1]);
			tab_cursor = tab_index;
		}

	/*
	 * --focus N moves the cursor into the active pane before rendering, so a
	 * single frame can show what the focused row looks like. Without it the
	 * cursor sits on the tab bar and no row is marked.
	 */
	int focus_row = -1;
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], "--focus") == 0)
			focus_row = atoi(argv[i + 1]);
	if (focus_row >= 0) {
		sheet_container->SetActiveChild(tab_body.get());
		Component pane = tab_index == 1 ? spell_controls : attrs_controls;
		if (focus_row < (int)pane->ChildCount())
			pane->SetActiveChild(pane->ChildAt(focus_row).get());
	}

	if (want_render) {
		auto doc = (screen_index == 0 ? picker : sheet)->Render();
		/* flex resolves to its minimum under Fit, which silently drops the
		 * lower half of the sheet; give the frame a real height instead. */
		auto out = Screen::Create(Dimension::Fixed(78), Dimension::Fixed(44));
		Render(out, doc);
		out.Print();
		printf("\n");
		return 0;
	}

	auto screen = ScreenInteractive::Fullscreen();
	screen.Loop(app);
	return 0;
}
