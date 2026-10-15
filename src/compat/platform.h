/**
 * @file platform.h
 *
 * Everything butcher does that POSIX and Windows spell differently, in one
 * place, so the rest of the code says what it means once.
 *
 * Deliberately free of compat/shim.h: the Windows side includes <windows.h>,
 * whose BYTE/DWORD/BOOL would collide with the shim's.
 *
 * Paths are UTF-8 everywhere. On Windows that holds because butcher.exe
 * carries a manifest setting the process code page to UTF-8 (Windows 10 1903
 * and later), so the narrow-path C runtime and the *A Win32 calls below take
 * UTF-8 as they are -- see src/compat/butcher.manifest.
 */
#ifndef BUTCHER_PLATFORM_H
#define BUTCHER_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Nonzero for a path separator: '/' everywhere, and '\\' on Windows too. */
int plat_is_sep(char c);

/** The last separator in @p path, or NULL if there is none. */
const char *plat_last_sep(const char *path);

/** Nonzero if @p path names an existing directory. */
int plat_is_dir(const char *path);

/**
 * Create @p path for binary writing, failing if anything is already there --
 * fopen(path, "wbx"). Never replaces an existing file, which is the whole
 * reason backups and new saves use it. On failure errno says why.
 */
FILE *plat_fopen_new(const char *path);

/**
 * Create a uniquely named temporary file beside @p path, open for binary
 * writing, and write its name into @p tmp (@p n bytes).
 *
 * Carries over @p path's permission bits where the platform has them: a
 * freshly created temporary is private, and the rename onto @p path would
 * otherwise carry that onto the save.
 *
 * @return NULL with errno set on failure.
 */
FILE *plat_temp_beside(const char *path, char *tmp, size_t n);

/** Flush @p f and force it to the disk. @return 0, or -1 with errno set. */
int plat_sync(FILE *f);

/**
 * Replace @p dst with @p src in one step, so a reader sees one file or the
 * other and never neither. POSIX rename() already does; Windows' does not
 * replace an existing file, so there it is MoveFileEx with
 * MOVEFILE_REPLACE_EXISTING.
 *
 * @return 0, or -1 with errno set.
 */
int plat_replace(const char *src, const char *dst);

/**
 * Process start-up for a console program: binary stdin and stdout (Windows
 * would otherwise translate bytes in a save piped through them) and UTF-8
 * console output. Nothing to do on POSIX.
 */
void plat_console_init(void);

/** stdout buffering that flushes per line, or as close as the platform has. */
void plat_stdout_line_buffered(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTCHER_PLATFORM_H */
