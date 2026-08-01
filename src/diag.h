/**
 * @file diag.h
 *
 * A collected list of findings, so that checking a document reports every
 * problem at once instead of stopping at the first.
 *
 * Two severities, and the distinction is the whole point:
 *
 *   ERROR   -- importing this would produce a character the game mishandles,
 *              or would silently lose what the document says. Refused.
 *   WARNING -- legal, and the game will load it, but it is probably not what
 *              the author meant. Reported and allowed.
 */
#ifndef BUTCHER_DIAG_H
#define BUTCHER_DIAG_H

#include <stddef.h>

#define DIAG_MSG_LEN 320
#define DIAG_WHERE_LEN 96

typedef enum DiagLevel {
	DIAG_ERROR = 0,
	DIAG_WARNING = 1
} DiagLevel;

typedef struct Diag {
	DiagLevel level;
	char where[DIAG_WHERE_LEN]; /**< dotted path, or "" when not positional */
	char msg[DIAG_MSG_LEN];
} Diag;

typedef struct DiagList {
	Diag *items;
	int n;
	int cap;
} DiagList;

void dl_init(DiagList *dl);
void dl_free(DiagList *dl);

/** Append a finding. @p where may be NULL. */
void dl_add(DiagList *dl, DiagLevel level, const char *where, const char *fmt, ...);

int dl_count(const DiagList *dl, DiagLevel level);
static inline int dl_has_errors(const DiagList *dl)
{
	return dl_count(dl, DIAG_ERROR) > 0;
}

/** Print every finding, errors first. @return number of errors. */
int dl_report(const DiagList *dl, const char *source, void *stream);

/** Longest-common-prefix style nearest match, for "did you mean" hints. */
const char *dl_nearest(const char *word, const char *const *candidates, int n);

#endif /* BUTCHER_DIAG_H */
