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
 *   butcher <save> --rows N       render at that height, to see a small window
 *   butcher <dir>  --new          open the create form before rendering
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
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"

#include <sys/stat.h>

#include "../src/compat/platform.h"

#include <algorithm>
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
#ifdef _WIN32
	size_t slash = path.find_last_of("/\\");
#else
	size_t slash = path.find_last_of('/');
#endif
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/*
 * The height --render draws onto, which --rows overrides. It is a variable
 * rather than a constant because Windowed() has to assume it: outside the
 * event loop there is no screen to ask how tall the terminal is, and a short
 * render is the only way to see what a short terminal would do.
 */
int render_rows = 44;

/*
 * The height each screen stops shrinking at. Below it the terminal becomes a
 * window onto a layout that keeps its full size.
 *
 * The sheet spends about twelve rows on chrome -- header, path, tab bar,
 * diagnostics, status, key hints and the border -- so this leaves a handful
 * for the pane itself. The picker spends six.
 */
const int kSheetMinRows = 18;
const int kPickerMinRows = 9;

/**
 * Stop shrinking; start scrolling.
 *
 * FTXUI's vbox has no floor. When the space runs out it takes it from every
 * child in proportion (box_helper::ComputeShrinkHard), so a short terminal
 * does not clip a screen, it compresses all of it at once and rows quietly
 * stop being drawn -- including the key hints that would say how to reach
 * what went missing. Past `rows` the layout keeps its height and the frame
 * scrolls to whatever holds `focus`, which MarkRow already puts on the
 * cursor row.
 *
 * Height only. The sliders are `flex` and degrade gracefully in a narrow
 * window, and a short slider is worth more than one that keeps its length by
 * pushing its own label off the side.
 */
Element Windowed(Element body, int rows)
{
	ScreenInteractive *active = ScreenInteractive::Active();
	/* Less the border, which is applied outside this and always drawn. */
	int avail = (active != nullptr ? active->dimy() : render_rows) - 2;
	/*
	 * The filler takes the spare row. Frame measures a box as max-min rather
	 * than as a count, so it hands its child one row more than it can draw,
	 * and it will not scroll to the last of them. Left to itself the body
	 * spends that row on its one flexible part and everything below shifts
	 * out of reach -- the key hints could not be scrolled back to. Given
	 * somewhere else to go, the body keeps the height asked for here and the
	 * bottom row stays reachable.
	 */
	return vbox({
	           body | size(HEIGHT, EQUAL, std::max(rows, avail)),
	           filler(),
	       })
	    | yframe;
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
 * Take away a row's claim on the frame.
 *
 * FTXUI widgets mark themselves with `focus` whether or not they hold the
 * cursor: an Input always does (input.cpp renders the unfocused case with a
 * plain `focus`), and a Radiobox always marks its selected entry. A frame
 * scrolls to the mark its vbox kept, and vbox keeps the first one it was
 * given unless a later one belongs to the focused component -- which none of
 * these do, since the row is drawn from an element, not from the component.
 *
 * So the Name field, being the first row of the attributes pane, held the
 * pane at the top for good: the cursor could be moved down to Gold or Dungeon
 * and the view would not follow, which is the whole point of the frame. The
 * spells pane scrolled only because it has neither widget in it.
 *
 * Hiding the mark of every row that is not the cursor leaves MarkRow's own as
 * the only one, whatever the rows are built from.
 */
Element Unfocusable(Element child)
{
	class Impl : public Node {
	public:
		explicit Impl(Element c)
		    : Node({ std::move(c) })
		{
		}
		void ComputeRequirement() override
		{
			Node::ComputeRequirement();
			requirement_.focused = Requirement::Focused();
		}
		void SetBox(Box box) override
		{
			Node::SetBox(box);
			children_[0]->SetBox(box);
		}
	};
	return std::make_shared<Impl>(std::move(child));
}

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
	if (!focused)
		row = Unfocusable(std::move(row));
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

/* ------------------------------------------------------------------ */
/* Finding saves                                                       */
/* ------------------------------------------------------------------ */

/** What the command line asked for, and the saves it led to. */
struct Launch {
	std::vector<SaveEntry> saves;
	char used_dir[SAVE_PATH_MAX] = { 0 };
	/*
	 * Only naming a save file opens it directly. Pointing at a directory
	 * means "show me what is here" -- jumping into whichever character
	 * happened to sort first is the tool choosing for you, and with one save
	 * in the folder it is not even obvious that it chose.
	 */
	bool named_one = false;
	bool want_render = false;
};

/**
 * Work out which saves to show.
 * @return -1 to carry on into the interface, or the status to exit with.
 */
int FindSaves(int argc, char **argv, Launch *out)
{
	std::vector<SaveEntry> &saves = out->saves;
	saves.resize(SAVE_MAX_SLOTS);
	char *used_dir = out->used_dir;
	int nsaves = 0;
	bool want_saves = false;
	const char *arg = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--render") == 0)
			out->want_render = true;
		else if (strcmp(argv[i], "--saves") == 0)
			want_saves = true;
		/* Skip the value of a flag that takes one, or it is taken for the
		 * path -- `--tab 1 saves/` used to open a directory named "1". */
		else if (strcmp(argv[i], "--tab") == 0 || strcmp(argv[i], "--focus") == 0
		    || strcmp(argv[i], "--rows") == 0)
			i++;
		else if (strncmp(argv[i], "--", 2) == 0)
			continue; /* the flavour flags */
		else if (arg == nullptr)
			arg = argv[i];
	}

	if (arg != nullptr) {
		if (plat_is_dir(arg)) {
			nsaves = save_scan_dir(arg, saves.data(), SAVE_MAX_SLOTS);
			snprintf(used_dir, SAVE_PATH_MAX, "%s", arg);
		} else {
			out->named_one = true;
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
		snprintf(used_dir, SAVE_PATH_MAX, ".");
	}

	if (nsaves == 0) {
		fprintf(stderr, "butcher: no readable saves found%s%s\n",
		    used_dir[0] ? " in " : "", used_dir);
		fprintf(stderr, "pass a save file or a directory to look in.\n");
		return 1;
	}
	saves.resize(nsaves);
	return -1;
}

/* ------------------------------------------------------------------ */
/* The interface                                                       */
/* ------------------------------------------------------------------ */

/**
 * Both screens -- the picker and the character sheet -- and the keys that
 * move between them.
 *
 * Every FTXUI component here binds to the address of a member, so a Tui is
 * built in place and never copied or moved. Construction order matters for
 * the same reason: `ed` holds a real character before any component that
 * points into it exists.
 */
class Tui {
public:
	explicit Tui(Launch &launch);
	Tui(const Tui &) = delete;
	Tui &operator=(const Tui &) = delete;

	/** --new, --tab, --rows and --focus: set the scene for --render. */
	void ApplyFlags(int argc, char **argv);
	/** Draw one frame to stdout. */
	void RenderOnce();
	void Loop();

private:
	/* ---- building ---- */
	void BuildPicker();
	void BuildAttributes();
	void BuildSpells();
	void BuildInventory();
	void BuildSheet();

	/* ---- drawing ---- */
	Element DrawPicker();
	Element DrawAttributes();
	Element DrawSpells();
	Element DrawInventory();
	Element DrawSheet();

	/* ---- keys ---- */
	bool OnEvent(Event e);
	bool OnConfirm(Event e);
	bool OnPicker(Event e);
	bool OnCreate(Event e);
	bool OnSheet(Event e);
	/** Ctrl+Y on the confirmation: write the character. */
	void Save();
	/** Return on the create form: make the character and open it. */
	void Create();

	/* ---- helpers ---- */
	/** Load entry @p i onto the sheet and switch to it, cursor on the tab bar. */
	void OpenSheet(int i);
	void ReloadClasses();
	void ReloadNewClasses();
	HeroFlavor NewFlavor() const { return new_flavor == 1 ? FLAVOR_HELLFIRE : FLAVOR_DIABLO; }
	static std::string PickerLine(const SaveEntry &s);

	/* ---- shared state ---- */
	std::vector<SaveEntry> &saves;
	const char *used_dir;
	bool named_one;

	Editor ed;
	int screen_index; /* 0 picker, 1 sheet */
	int picked = 0;
	int tab_index = 0;
	bool name_was_focused = false;
	bool backup = true;
	bool confirming = false;
	std::string status;
	Color status_color = Color::GrayLight;

	/* ---- picker and the new-character form ---- */
	std::vector<std::string> picker_items;
	bool any_in_progress = false;
	Component picker_menu;
	bool creating = false;
	int creating_tab = 0;
	std::string new_name;
	int new_class = 0;
	int new_flavor = 0; /* 0 Diablo, 1 Hellfire */
	std::string new_error;
	std::vector<std::string> flavor_names { "Diablo", "Hellfire" };
	std::vector<std::string> new_class_names;
	int new_cursor = 0;
	Component new_name_input, new_flavor_pick, new_class_pick, create_form;
	Component picker;

	/* ---- attributes pane ---- */
	std::vector<std::string> class_names;
	int name_cursor = 0;
	Component name_input, name_field, class_pick;
	Component slider_str, slider_mag, slider_dex, slider_vit, slider_pts, slider_lvl;
	Component slider_hp, slider_hpmax, slider_mana, slider_manamax, slider_gold;
	int cap_stat_pts = 255;
	int cap_level = hero_max_level();
	Component attrs_controls, attrs_pane;

	/* ---- spells pane ---- */
	std::vector<int> spell_ids;
	std::vector<Component> spell_sliders;
	int cap_spell = 15;
	Component spell_controls, spells_pane;

	/* ---- inventory pane ---- */
	/* The last two are measured while drawing: how tall the pane came out,
	 * and how much of it there was room for. */
	int inv_scroll = 0;
	int inv_rows = 0;
	Box inv_view {};
	Component inventory_pane;

	/* ---- sheet ---- */
	std::vector<std::string> tab_names = { "Attributes", "Spells", "Inventory" };
	int tab_cursor = 0;
	Component tab_bar, tab_body, sheet_container, sheet;

	Component app;
};

Tui::Tui(Launch &launch)
    : saves(launch.saves)
    , used_dir(launch.used_dir)
    , named_one(launch.named_one)
    , screen_index(launch.named_one ? 1 : 0)
{
	/*
	 * Load unconditionally, even when the picker is shown first. The
	 * components below bind to addresses inside `ed`, so it must hold a real
	 * character before any of them exist.
	 */
	ed.Load(saves[0]);

	BuildPicker();
	BuildAttributes();
	BuildSpells();
	BuildInventory();
	BuildSheet();

	auto root_container = Container::Tab({ picker, sheet }, &screen_index);
	app = CatchEvent(root_container, [this](Event e) { return OnEvent(e); });
}

std::string Tui::PickerLine(const SaveEntry &s)
{
	/* The "!" marks a save whose character the game loads from "game"
	 * rather than from the "hero" this edits. */
	return std::string(s.in_progress ? "! " : "  ") + Pad(s.name, 18)
	    + Pad(hero_flavor_name(s.flavor), 10)
	    + Pad(hero_class_name(s.flavor, s.hero.pClass), 11) + "lvl "
	    + Pad(std::to_string(s.hero.pLevel), 4) + Commas(s.hero.pGold) + " gold";
}

void Tui::ReloadNewClasses()
{
	new_class_names.clear();
	for (int i = 0; i < hero_num_classes(NewFlavor()); i++)
		new_class_names.push_back(hero_class_name(NewFlavor(), i));
	/* Diablo has three; switching down from a Hellfire class must not
	 * leave the selection past the end. */
	if (new_class >= (int)new_class_names.size())
		new_class = 0;
}

/* Rebuilt whenever a character is opened: Hellfire has six classes, Diablo
 * three, and the list must match the save actually in front of you. */
void Tui::ReloadClasses()
{
	class_names.clear();
	for (int i = 0; i < hero_num_classes(ed.entry.flavor); i++)
		class_names.push_back(hero_class_name(ed.entry.flavor, i));
}

/* ---- picker ---- */

void Tui::BuildPicker()
{
	for (const auto &s : saves) {
		picker_items.push_back(PickerLine(s));
		if (s.in_progress)
			any_in_progress = true;
	}
	picker_menu = Menu(&picker_items, &picked);

	ReloadNewClasses();
	InputOption new_name_opt = InputOption::Default();
	new_name_opt.multiline = false;
	new_name_opt.cursor_position = &new_cursor;
	new_name_opt.on_change = [this] { new_name = editor_sanitize_name(new_name); };
	new_name_input = Input(&new_name, "name", new_name_opt);
	new_flavor_pick = Radiobox(&flavor_names, &new_flavor);
	new_class_pick = Radiobox(&new_class_names, &new_class);

	create_form = Container::Vertical({
	    NavEscape(new_name_input),
	    new_flavor_pick,
	    new_class_pick,
	});

	auto picker_body = Container::Tab({ picker_menu, create_form }, &creating_tab);
	picker = Renderer(picker_body, [this] { return DrawPicker(); });
}

Element Tui::DrawPicker()
{
	Element list_body = vbox({
	                   text(" butcher ") | bold,
	                   text(std::string(" ") + used_dir) | dim,
	                   separator(),
	                   picker_menu->Render() | frame | flex,
	                   separator(),
	                   any_in_progress
	                       ? text(" ! has a game in progress; edits are applied "
	                              "to the saved game too ")
	                           | color(Color::Cyan)
	                       : text(""),
	                   text(" ↑↓ choose · enter open · n new · q quit ") | dim,
	               });
	Element list = Windowed(list_body, kPickerMinRows) | border;

	if (!creating)
		return list;

	ReloadNewClasses();
	Element form = vbox({
	                   text(" New character ") | bold,
	                   separator(),
	                   hbox({ text("  Name   "), new_name_input->Render() | flex }),
	                   hbox({ text("  Game   "), new_flavor_pick->Render() }),
	                   hbox({ text("  Class  "), new_class_pick->Render() }),
	                   separator(),
	                   text("  no equipment -- the game builds starting gear")
	                       | dim,
	                   text("  from item seeds, which cannot be made here.")
	                       | dim,
	                   text("  give it gold on the sheet and buy some.") | dim,
	                   new_error.empty()
	                       ? text("")
	                       : paragraph("  " + new_error) | color(Color::Red),
	                   separator(),
	                   text(" enter create · esc cancel ") | dim,
	               })
	    | border | bgcolor(Color::Black);
	return dbox({ list, form | center });
}

/* ---- attributes pane ---- */

void Tui::BuildAttributes()
{
	ReloadClasses();

	/*
	 * Single line, and sanitised as it is typed. The UI should not accept
	 * something validation will then reject: Return used to insert a newline,
	 * and hero_name_valid refuses control characters.
	 */
	InputOption name_opt = InputOption::Default();
	name_opt.multiline = false;
	name_opt.cursor_position = &name_cursor;
	name_opt.on_change = [this] {
		std::string clean = editor_sanitize_name(ed.name);
		if (clean != ed.name)
			ed.name = clean;
	};
	name_input = Input(&ed.name, "name", name_opt);
	/* Ctrl+U clears the field, so a name can be replaced rather than edited. */
	name_field = NavEscape(CatchEvent(name_input, [this](Event e) {
		if (e == Event::CtrlU) {
			ed.name.clear();
			name_cursor = 0;
			return true;
		}
		return false;
	}));
	class_pick = Radiobox(&class_names, &ed.cls);

	slider_str = Slider(SliderOption<int> { &ed.str, 0, &ed.cap_str, 1 });
	slider_mag = Slider(SliderOption<int> { &ed.mag, 0, &ed.cap_mag, 1 });
	slider_dex = Slider(SliderOption<int> { &ed.dex, 0, &ed.cap_dex, 1 });
	slider_vit = Slider(SliderOption<int> { &ed.vit, 0, &ed.cap_vit, 1 });
	slider_pts = Slider(SliderOption<int> { &ed.statpts, 0, &cap_stat_pts, 1 });
	/*
	 * The floor is the stored level: the game never takes a level back, so
	 * offering to lower one would promise something it cannot do. on_change
	 * applies the level's rewards to the other sliders as it moves.
	 */
	SliderOption<int> lvl_opt { &ed.level_target, 1, &cap_level, 1 };
	lvl_opt.min = &ed.level_stored;
	lvl_opt.on_change = [this] { ed.ApplyLevelTarget(); };
	slider_lvl = Slider(lvl_opt);
	slider_hp = Slider(SliderOption<int> { &ed.hp, 1, &ed.hp_max, 1 });
	slider_hpmax = Slider(SliderOption<int> { &ed.hp_max, 1, &ed.cap_life, 1 });
	slider_mana = Slider(SliderOption<int> { &ed.mana, 0, &ed.mana_max, 1 });
	slider_manamax = Slider(SliderOption<int> { &ed.mana_max, 0, &ed.cap_mana, 1 });
	/*
	 * Gold moves a pile at a time. A step of 100 meant dozens of presses to
	 * reach a useful figure and landed on totals like 2,654 that no character
	 * would ever have. Snapping down to a multiple after each step makes the
	 * first press from any odd amount land on GOLD_STEP exactly, and every
	 * press after that add exactly GOLD_STEP.
	 */
	static const int kGoldStep = 5000;
	SliderOption<int> gold_opt { &ed.gold, 0, &ed.cap_gold, kGoldStep };
	gold_opt.on_change = [this] {
		int snapped = ed.gold - (ed.gold % kGoldStep);
		/* Never snap a non-zero amount away to nothing. */
		ed.gold = (snapped == 0 && ed.gold > 0) ? kGoldStep : snapped;
		if (ed.gold > ed.cap_gold)
			ed.gold = ed.cap_gold;
	};
	slider_gold = Slider(gold_opt);
	/*
	 * There is no dungeon slider. plrlevel is a record of where the character
	 * was, not an instruction about where to start: SetupLocalCoords does
	 * `plr[myplr].plrlevel = currlevel` with currlevel forced to 0, so a new
	 * game always begins in town, and a game in progress takes its level from
	 * the saved game instead. It is shown below, and left alone.
	 */

	attrs_controls = Container::Vertical({
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
	});

	attrs_pane = Renderer(attrs_controls, [this] { return DrawAttributes(); });
}

Element Tui::DrawAttributes()
{
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
	 * Levelling is applied outright, so the note reports what was granted
	 * rather than what is pending. It used to promise the game would award
	 * the levels on the next kill; it never would -- ValidatePlayer clamps
	 * experience back down every tick.
	 */
	int gained = ed.level_target - ed.level_stored;
	if (gained > 0) {
		int hp_per = (ed.cls == PC_SORCERER ? 64 : 128) + 1;
		int mana_per = (ed.cls == PC_WARRIOR ? 64 : ed.cls == 5 ? 0 : 128) + 1;
		rows.push_back(text("   └ levels " + std::to_string(ed.level_stored + 1)
		                  + "-" + std::to_string(ed.level_target) + " granted: +"
		                  + std::to_string(ed.statpts) + " pts, +"
		                  + std::to_string((gained * hp_per) >> HERO_FIXED_SHIFT)
		                  + " life, +"
		                  + std::to_string((gained * mana_per) >> HERO_FIXED_SHIFT)
		                  + " mana")
		    | color(Color::Cyan));
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
	/* A Hellfire pile holds twice a Diablo one, so this cannot be a
	 * constant -- it read "at most 5,000" on a Hellfire character. */
	int stack_max = hero_gold_stack_max(ed.entry.flavor, &h);
	int nstacks = (ed.gold + stack_max - 1) / stack_max;
	rows.push_back(text("   └ " + std::to_string(nstacks)
	                  + (nstacks == 1 ? " stack of at most " : " stacks of at most ")
	                  + Commas(stack_max)
	                  + ", " + std::to_string(hero_free_inv_cells(&h))
	                  + " cells free")
	    | dim);
	/*
	 * Reported, not offered. This was a slider until it turned out the
	 * game never reads it: setting it to 5 and loading still started the
	 * character in town. It is still worth showing -- it says whether the
	 * character is standing in a dungeon -- so it stays as a line.
	 */
	rows.push_back(separator());
	rows.push_back(hbox({
	    text("  " + Pad("Dungeon", 11)),
	    text(ed.dlvl == 0 ? "town" : std::to_string(ed.dlvl)),
	}));
	rows.push_back(text("   └ where the save says the character is; the game "
	                    "decides this on load")
	    | dim);

	/*
	 * Scrolled, not squeezed. This is the only `flex` child of the sheet,
	 * so it is the first thing a short window takes room from, and without
	 * a frame it lost rows from the middle of the list -- Gold and Dungeon
	 * were simply absent, with nothing to say they existed. MarkRow puts
	 * `focus` on the cursor row, which is what the frame scrolls to.
	 */
	return vbox(rows) | vscroll_indicator | yframe;
}

/* ---- spells pane ---- */

void Tui::BuildSpells()
{
	/*
	 * Every id either game names, so the component set is fixed for the life
	 * of the program. Rows the current flavour does not define are wrapped in
	 * Maybe so they are skipped for focus as well as hidden -- hiding them at
	 * render time alone left them reachable, which is how a Hellfire spell got
	 * toggled into a Diablo save and made the character unloadable.
	 */
	for (int s = 1; s < hero_spell_persisted(); s++)
		if (hero_spell_name(FLAVOR_DIABLO, s) != nullptr
		    || hero_spell_name(FLAVOR_HELLFIRE, s) != nullptr)
			spell_ids.push_back(s);

	spell_controls = Container::Vertical({});
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
		auto row = CatchEvent(Slider(opt), [this, s](Event e) {
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
		    Maybe(row, [this, s] { return hero_spell_exists(ed.entry.flavor, s) != 0; }));
	}

	spells_pane = Renderer(spell_controls, [this] { return DrawSpells(); });
}

Element Tui::DrawSpells()
{
	std::vector<Element> rows;
	rows.push_back(hbox({
	                   text("   "),                /* marker + glyph */
	                   text(" " + Pad("spell", 18)),
	                   text(Pad("level", 4)),
	               })
	    | dim);
	rows.push_back(text("   space toggles the spell book; · marks a spell "
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
	/* Height only: a horizontal frame would pan the label out of view to
	 * keep a slider at full length, which is the wrong trade. */
	return vbox(rows) | vscroll_indicator | yframe;
}

/* ---- inventory pane (read only) ---- */

void Tui::BuildInventory()
{
	/*
	 * Focusable even though it holds no controls. A Renderer with no children
	 * is not, which made the whole tab body unfocusable while Inventory was
	 * selected -- so an arrow press there moved focus up to the tab bar and
	 * left the panes with nothing focused.
	 */
	auto inventory_view = Renderer([this](bool) { return DrawInventory(); });

	/*
	 * Arrows scroll the pane. Each is taken only when it has somewhere to go:
	 * at the top Up still passes through to the tab bar, as it did when this
	 * pane handled nothing, and when the whole inventory already fits neither
	 * key is swallowed -- a cursor with no visible effect reads as a dead key.
	 */
	inventory_pane = CatchEvent(inventory_view, [this](Event e) {
		int max_scroll = inv_rows - (inv_view.y_max - inv_view.y_min + 1);
		if (max_scroll < 0)
			max_scroll = 0;
		if (inv_scroll > max_scroll)
			inv_scroll = max_scroll;
		if (e == Event::ArrowDown && inv_scroll < max_scroll) {
			inv_scroll++;
			return true;
		}
		if (e == Event::ArrowUp && inv_scroll > 0) {
			inv_scroll--;
			return true;
		}
		return false;
	});
}

Element Tui::DrawInventory()
{
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

	/*
	 * A column of empty rows, one per line of the pane, carrying `focus`
	 * at the scroll position. Nothing here is a widget and the two
	 * columns are different lengths, so the frame has nothing of its own
	 * to scroll to; this gives it a mark that does not depend on which
	 * column is longer, and costs no width -- an empty text is zero
	 * columns wide. It goes first, because an hbox keeps the first mark
	 * it is handed, and it spans the note at the foot as well, or the
	 * last rows could not be scrolled to.
	 */
	int rows = std::max((int)left.size(), 1 + 4 + 1 + shown) + 2;
	std::vector<Element> ruler;
	for (int i = 0; i < rows; i++)
		ruler.push_back(i == inv_scroll ? text("") | focus : text(""));
	inv_rows = rows;

	return hbox({
	           vbox(ruler),
	           vbox({
	               hbox({ vbox(left) | flex, separator(), vbox(right) | flex }),
	               separator(),
	               text("  items are generated from a seed and cannot be "
	                    "edited; shown for reference")
	                   | dim,
	           })
	               | flex,
	       })
	    | vscroll_indicator | yframe | reflect(inv_view);
}

/* ---- sheet ---- */

void Tui::BuildSheet()
{
	/*
	 * A Menu tracks two positions: which entry is selected, and where its own
	 * cursor sits. Toggle(&names, &index) only exposes the first, so changing
	 * the index from a key handler moved the selection while leaving the
	 * cursor behind -- the bar then showed one tab bold and a different one
	 * reversed. Binding both means they cannot disagree.
	 */
	MenuOption tab_opt = MenuOption::Toggle();
	tab_opt.entries = &tab_names;
	tab_opt.selected = &tab_index;
	tab_opt.focused_entry = &tab_cursor;
	tab_bar = Menu(tab_opt);
	tab_body = Container::Tab({ attrs_pane, spells_pane, inventory_pane }, &tab_index);
	sheet_container = Container::Vertical({ tab_bar, tab_body });
	sheet = Renderer(sheet_container, [this] { return DrawSheet(); });
}

Element Tui::DrawSheet()
{
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

	Element framed = Windowed(body, kSheetMinRows) | border;

	if (!confirming)
		return framed;

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
	return dbox({ framed, ask | center });
}

/* ---- keys ---- */

bool Tui::OnEvent(Event e)
{
	if (confirming)
		return OnConfirm(e);
	if (screen_index == 0)
		return creating ? OnCreate(e) : OnPicker(e);
	return OnSheet(e);
}

bool Tui::OnConfirm(Event e)
{
	if (e == Event::Character('y')) {
		Save();
		confirming = false;
		return true;
	}
	if (e == Event::Character('n') || e == Event::Escape) {
		confirming = false;
		return true;
	}
	return true; /* swallow everything else while asking */
}

void Tui::Save()
{
	PkPlayerStruct h = ed.Compose();
	char why[HERO_ERR_LEN];
	if (!ed.CanSave(why, sizeof(why))) {
		status = std::string("refused: ") + why;
		status_color = Color::Red;
		return;
	}

	char werr[MPQ_ERR_LEN];
	char bak[SAVE_PATH_MAX] = { 0 };
	bool ok = true;
	if (backup)
		ok = save_backup_to(ed.entry.path, bak, sizeof(bak), werr) != 0;
	SaveGameSync sync = SAVE_GAME_ABSENT;
	if (ok)
		ok = save_commit_ex(ed.entry.path, &h, /*backup=*/0, &sync, werr) != 0;
	if (!ok) {
		status = std::string("failed: ") + werr;
		status_color = Color::Red;
		return;
	}

	/*
	 * entry.hero is what "unchanged" is measured against, so it has to become
	 * the character just written. Leaving it as the file first read meant the
	 * sheet still said "modified" immediately after a successful save -- which
	 * reads as the save not having happened, and invites a second one that
	 * writes another backup for nothing.
	 */
	ed.original = h;
	ed.entry.hero = h;
	ed.level_opened = ed.level_target;
	ed.level_stored = h.pLevel;
	ed.gold_opened = ed.gold;
	/* Name the backup: it is not always <save>.bak, and the user needs to
	 * know which file to restore from. */
	status = backup ? std::string("saved -- backup ") + Leaf(bak) : std::string("saved");
	if (sync == SAVE_GAME_SYNCED)
		status += " (saved game updated too)";
	else if (sync == SAVE_GAME_SYNCED_ITEMS)
		status += " (saved game and inventory updated too)";
	status_color = Color::Green;
}

void Tui::OpenSheet(int i)
{
	ed.Load(saves[i]);
	ReloadClasses();
	status.clear();
	/*
	 * Start on the tab bar: Right switches pane, Down enters it. The bar's
	 * cursor moves with the selection, or reopening a character after
	 * leaving one on another pane drew one tab bold and a different one
	 * reversed.
	 */
	tab_index = 0;
	tab_cursor = 0;
	sheet_container->SetActiveChild(tab_bar.get());
	attrs_controls->SetActiveChild(name_field.get());
	screen_index = 1;
}

bool Tui::OnPicker(Event e)
{
	if (e == Event::Character('n')) {
		creating = true;
		creating_tab = 1;
		new_error.clear();
		new_name.clear();
		new_cursor = 0;
		create_form->SetActiveChild(create_form->ChildAt(0).get());
		return true;
	}
	if (e == Event::Return) {
		OpenSheet(picked);
		return true;
	}
	if (e == Event::Character('q')) {
		ScreenInteractive::Active()->Exit();
		return true;
	}
	return false;
}

bool Tui::OnCreate(Event e)
{
	if (e == Event::Escape) {
		creating = false;
		creating_tab = 0;
		new_error.clear();
		return true;
	}
	if (e == Event::Return) {
		Create();
		return true;
	}
	return false; /* the form handles the rest */
}

void Tui::Create()
{
	new_error.clear();
	HeroFlavor f = NewFlavor();

	/* The first slot the game would not already find taken. */
	char path[SAVE_PATH_MAX] = { 0 };
	for (int i = 0; i < MAX_CHARACTERS; i++) {
		char cand[SAVE_PATH_MAX];
		snprintf(cand, sizeof(cand), "%s/single_%d%s", used_dir, i,
		    f == FLAVOR_HELLFIRE ? ".hsv" : ".sv");
		struct stat st;
		if (stat(cand, &st) != 0) {
			snprintf(path, sizeof(path), "%s", cand);
			break;
		}
	}
	if (path[0] == '\0') {
		new_error = "all ten slots for that game are taken";
		return;
	}

	PkPlayerStruct nh;
	char err[MPQ_ERR_LEN];
	if (!hero_create(&nh, f, new_class, new_name.c_str(), err)) {
		new_error = err;
		return;
	}
	char clash[SAVE_PATH_MAX];
	if (save_name_collides(path, nh.pName, clash, sizeof(clash))) {
		new_error = std::string(Leaf(clash)) + " already uses that name";
		return;
	}
	if (!save_create(path, &nh, err)) {
		new_error = err;
		return;
	}

	/* Add it to the list and open it, so the new character is the one on
	 * screen rather than something to go find. */
	SaveEntry ne {};
	snprintf(ne.path, sizeof(ne.path), "%s", path);
	memcpy(ne.name, nh.pName, PLR_NAME_LEN);
	ne.name[PLR_NAME_LEN] = '\0';
	ne.slot = (int)saves.size();
	ne.flavor = f;
	ne.in_progress = 0;
	ne.hero = nh;
	saves.push_back(ne);
	picker_items.push_back(PickerLine(ne));
	picked = (int)saves.size() - 1;

	creating = false;
	creating_tab = 0;
	new_name.clear();
	OpenSheet(picked);
	status = "created " + std::string(Leaf(path))
	    + " -- no equipment; give it gold and buy some";
	status_color = Color::Cyan;
}

bool Tui::OnSheet(Event e)
{
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
			ReloadClasses();
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
}

/* ---- running ---- */

void Tui::ApplyFlags(int argc, char **argv)
{
	/* --new opens the create form before rendering, like --tab for the sheet. */
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--new") == 0) {
			creating = true;
			creating_tab = 1;
		}

	/* --tab N selects a pane before rendering, so each can be inspected. */
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], "--tab") == 0) {
			tab_index = atoi(argv[i + 1]);
			tab_cursor = tab_index;
		}

	/*
	 * --rows N renders short, which is the only way to see what a small
	 * window does without one: a terminal cannot be resized from a test.
	 */
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], "--rows") == 0) {
			render_rows = atoi(argv[i + 1]);
			if (render_rows < 3)
				render_rows = 3; /* two of those are the border */
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
}

void Tui::RenderOnce()
{
	auto doc = (screen_index == 0 ? picker : sheet)->Render();
	/* flex resolves to its minimum under Fit, which silently drops the
	 * lower half of the sheet; give the frame a real height instead.
	 * Windowed() assumes this height when there is no screen to ask. */
	auto out = Screen::Create(Dimension::Fixed(78), Dimension::Fixed(render_rows));
	Render(out, doc);
	out.Print();
	printf("\n");
}

void Tui::Loop()
{
	auto screen = ScreenInteractive::Fullscreen();
	screen.Loop(app);
}

} // namespace

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

/* Entry point when no subcommand is given; see butcher.cpp for dispatch. */
int tui_main(int argc, char **argv)
{
	Launch launch;
	int rc = FindSaves(argc, argv, &launch);
	if (rc >= 0)
		return rc;

	Tui tui(launch);
	tui.ApplyFlags(argc, argv);
	if (launch.want_render)
		tui.RenderOnce();
	else
		tui.Loop();
	return 0;
}
