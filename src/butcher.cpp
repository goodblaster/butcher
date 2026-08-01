/**
 * @file butcher.cpp
 *
 * One binary, two interfaces.
 *
 * Naming a subcommand selects the command-line interface; anything else opens
 * the terminal UI. That keeps `butcher single_0.hsv` immediate while leaving
 * every scriptable operation exactly where it was.
 *
 * The two front ends share the library in src/ and neither invokes the other;
 * this file only chooses which one runs.
 */
#include <cstdio>
#include <cstring>

int cli_main(int argc, char **argv);
int tui_main(int argc, char **argv);

namespace {

/*
 * Every command-line verb. A first argument matching one of these goes to the
 * CLI; anything else is treated as a save or a directory for the UI.
 */
const char *const kVerbs[] = {
	"list", "show", "set", "validate", "export", "import", "dump", "patch",
	"help", "--help", "-h",
};

bool IsVerb(const char *arg)
{
	for (const char *v : kVerbs)
		if (strcmp(arg, v) == 0)
			return true;
	return false;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
		return tui_main(argc, argv); /* bare invocation browses saves */

	/*
	 * --tui forces the interface, for the unlikely case of a save file named
	 * after a subcommand. Strip it so the UI does not see it as a path.
	 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--tui") == 0) {
			for (int j = i; j + 1 < argc; j++)
				argv[j] = argv[j + 1];
			argc--;
			return tui_main(argc, argv);
		}
	}

	/*
	 * An explicit help request succeeds; only a malformed invocation fails.
	 * cli_main's usage() returns 2, which is right when a command was wrong
	 * and wrong when help was what was asked for.
	 */
	if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0
	    || strcmp(argv[1], "-h") == 0) {
		cli_main(argc, argv);
		return 0;
	}

	if (IsVerb(argv[1]))
		return cli_main(argc, argv);

	return tui_main(argc, argv);
}
