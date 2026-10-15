/**
 * @file portable.h
 *
 * The handful of POSIX calls the suites make that Windows spells differently.
 * Test-only: the product goes through src/compat/platform.h instead.
 */
#ifndef BUTCHER_TESTS_PORTABLE_H
#define BUTCHER_TESTS_PORTABLE_H

#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/** mkdir with the usual mode; Windows' takes no mode. */
static inline int test_mkdir(const char *path)
{
#ifdef _WIN32
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

/** Cut a file down to @p len bytes. */
static inline int test_truncate(const char *path, long len)
{
#ifdef _WIN32
	int fd = _open(path, _O_RDWR | _O_BINARY);
	if (fd < 0)
		return -1;
	int r = _chsize(fd, len);
	_close(fd);
	return r;
#else
	return truncate(path, (off_t)len);
#endif
}

/**
 * Run a shell command, capturing stdout into @p out.
 * @return its exit status, or -1 if it could not be run.
 */
static inline int test_run(const char *cmd, char *out, size_t n)
{
#ifdef _WIN32
	/*
	 * cmd.exe runs it, and cmd.exe reads "./build/butcher" as the command "."
	 * -- forward slashes are fine in arguments but not in the program name.
	 * The whole command is wrapped in one more pair of quotes because cmd /c
	 * strips the outermost pair.
	 */
	char fixed[2048];
	size_t i = 0;
	int in_prog = 1;
	fixed[i++] = '"';
	for (const char *c = cmd; *c != '\0' && i + 2 < sizeof(fixed); c++) {
		if (*c == ' ')
			in_prog = 0;
		fixed[i++] = (in_prog && *c == '/') ? '\\' : *c;
	}
	fixed[i++] = '"';
	fixed[i] = '\0';
	FILE *p = _popen(fixed, "rb");
#else
	FILE *p = popen(cmd, "r");
#endif
	if (p == NULL)
		return -1;
	size_t got = fread(out, 1, n - 1, p);
	out[got] = '\0';
#ifdef _WIN32
	return _pclose(p); /* already the exit code */
#else
	int rc = pclose(p);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

#endif /* BUTCHER_TESTS_PORTABLE_H */
