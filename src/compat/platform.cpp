/**
 * @file platform.cpp
 *
 * See platform.h. The POSIX half is what butcher always did; the Windows half
 * is its translation, and has only ever been exercised by CI.
 */
#include "platform.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

int plat_is_sep(char c)
{
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

const char *plat_last_sep(const char *path)
{
	const char *last = NULL;
	for (const char *p = path; *p != '\0'; p++) {
		if (plat_is_sep(*p))
			last = p;
	}
	return last;
}

int plat_is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

#ifdef _WIN32

/* Win32 reports GetLastError; the callers report strerror(errno). Map the
 * cases a user can act on, and let the rest read as an I/O error. */
static void set_errno_from_win32(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		errno = ENOENT;
		break;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION: /* the game has the save open */
	case ERROR_LOCK_VIOLATION:
		errno = EACCES;
		break;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		errno = EEXIST;
		break;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		errno = ENOSPC;
		break;
	default:
		errno = EIO;
		break;
	}
}

FILE *plat_fopen_new(const char *path)
{
	int fd = _open(path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
	    _S_IREAD | _S_IWRITE);
	if (fd < 0)
		return NULL;
	FILE *f = _fdopen(fd, "wb");
	if (f == NULL)
		_close(fd);
	return f;
}

FILE *plat_temp_beside(const char *path, char *tmp, size_t n)
{
	/* What mkstemp does: a name nobody has, created exclusively. */
	static unsigned counter;
	for (int attempt = 0; attempt < 100; attempt++) {
		unsigned r = (unsigned)GetCurrentProcessId() * 2654435761u
		    ^ (unsigned)GetTickCount() ^ (++counter * 40503u);
		snprintf(tmp, n, "%s.tmp%06X", path, r & 0xFFFFFF);
		FILE *f = plat_fopen_new(tmp);
		if (f != NULL)
			return f;
		if (errno != EEXIST)
			return NULL;
	}
	errno = EEXIST;
	return NULL;
}

int plat_sync(FILE *f)
{
	if (fflush(f) != 0)
		return -1;
	return _commit(_fileno(f)) == 0 ? 0 : -1;
}

int plat_replace(const char *src, const char *dst)
{
	/*
	 * WRITE_THROUGH: do not return until the move is on disk, the nearest
	 * thing to the fsync-then-rename the POSIX side does. Not ReplaceFile:
	 * that one merges attributes and ACLs and can leave a backup, where this
	 * wants exactly the temporary's bytes in the save's place.
	 */
	if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		return 0;
	set_errno_from_win32(GetLastError());
	return -1;
}

void plat_console_init(void)
{
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	SetConsoleOutputCP(CP_UTF8);
}

void plat_stdout_line_buffered(void)
{
	/* The Windows C runtime treats _IOLBF as full buffering, which is the
	 * very thing this exists to avoid: stderr warnings landing before the
	 * stdout they refer to. Unbuffered is the only per-line option there. */
	setvbuf(stdout, NULL, _IONBF, 0);
}

#else /* POSIX */

FILE *plat_fopen_new(const char *path)
{
	return fopen(path, "wbx");
}

FILE *plat_temp_beside(const char *path, char *tmp, size_t n)
{
	if (snprintf(tmp, n, "%s.tmpXXXXXX", path) >= (int)n) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	int fd = mkstemp(tmp);
	if (fd < 0)
		return NULL;

	/*
	 * mkstemp creates at 0600 and rename() carries that mode onto the
	 * target, so without this the save silently loses its permissions.
	 */
	struct stat st;
	if (stat(path, &st) == 0)
		(void)fchmod(fd, st.st_mode & 07777);

	FILE *f = fdopen(fd, "wb");
	if (f == NULL) {
		int e = errno;
		close(fd);
		remove(tmp);
		errno = e;
	}
	return f;
}

int plat_sync(FILE *f)
{
	if (fflush(f) != 0)
		return -1;
	return fsync(fileno(f));
}

int plat_replace(const char *src, const char *dst)
{
	return rename(src, dst);
}

void plat_console_init(void)
{
}

void plat_stdout_line_buffered(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
}

#endif
