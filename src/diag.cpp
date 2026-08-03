/**
 * @file diag.cpp
 *
 * Diagnostic collection. See diag.h.
 */
#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dl_init(DiagList *dl)
{
	dl->items = NULL;
	dl->n = 0;
	dl->cap = 0;
}

void dl_free(DiagList *dl)
{
	free(dl->items);
	dl_init(dl);
}

void dl_add(DiagList *dl, DiagLevel level, const char *where, const char *fmt, ...)
{
	if (dl == NULL)
		return;
	if (dl->n == dl->cap) {
		dl->cap = dl->cap ? dl->cap * 2 : 16;
		dl->items = (Diag *)realloc(dl->items, (size_t)dl->cap * sizeof(Diag));
	}
	Diag *d = &dl->items[dl->n++];
	d->level = level;
	snprintf(d->where, sizeof(d->where), "%s", where != NULL ? where : "");

	va_list va;
	va_start(va, fmt);
	vsnprintf(d->msg, sizeof(d->msg), fmt, va);
	va_end(va);
}

int dl_count(const DiagList *dl, DiagLevel level)
{
	int n = 0;
	for (int i = 0; i < dl->n; i++)
		if (dl->items[i].level == level)
			n++;
	return n;
}

int dl_report(const DiagList *dl, const char *source, void *stream)
{
	FILE *out = (FILE *)stream;
	int errors = dl_count(dl, DIAG_ERROR);
	int warnings = dl_count(dl, DIAG_WARNING);

	/* Errors first: they are what stops the import. */
	static const DiagLevel order[] = { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE };
	static const char *const labels[] = { "error", "warning", "fixed" };
	for (int pass = 0; pass < 3; pass++) {
		DiagLevel want = order[pass];
		for (int i = 0; i < dl->n; i++) {
			const Diag *d = &dl->items[i];
			if (d->level != want)
				continue;
			const char *label = labels[pass];
			if (source != NULL && d->where[0] != '\0')
				fprintf(out, "%s:%s: %s: %s\n", source, d->where, label, d->msg);
			else if (d->where[0] != '\0')
				fprintf(out, "%s: %s: %s\n", d->where, label, d->msg);
			else if (source != NULL)
				fprintf(out, "%s: %s: %s\n", source, label, d->msg);
			else
				fprintf(out, "%s: %s\n", label, d->msg);
		}
	}

	if (errors == 0 && warnings == 0)
		return 0;


	fprintf(out, "\n%d error%s, %d warning%s\n", errors, errors == 1 ? "" : "s",
	    warnings, warnings == 1 ? "" : "s");
	return errors;
}

/* ------------------------------------------------------------------ */

/** Levenshtein distance, capped -- only used to rank spelling suggestions. */
static int edit_distance(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	if (la > 64 || lb > 64)
		return 999;

	int prev[66], cur[66];
	for (size_t j = 0; j <= lb; j++)
		prev[j] = (int)j;

	for (size_t i = 1; i <= la; i++) {
		cur[0] = (int)i;
		for (size_t j = 1; j <= lb; j++) {
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int del = prev[j] + 1;
			int ins = cur[j - 1] + 1;
			int sub = prev[j - 1] + cost;
			int best = del < ins ? del : ins;
			cur[j] = best < sub ? best : sub;
		}
		memcpy(prev, cur, (lb + 1) * sizeof(int));
	}
	return prev[lb];
}

const char *dl_nearest(const char *word, const char *const *candidates, int n)
{
	const char *best = NULL;
	int best_d = 999;
	size_t wl = strlen(word);

	for (int i = 0; i < n; i++) {
		int d = edit_distance(word, candidates[i]);
		if (d < best_d) {
			best_d = d;
			best = candidates[i];
		}
	}

	/*
	 * Only suggest when the match is close enough to be plausible: within a
	 * third of the word's length, and never more than 3 edits. A wrong
	 * suggestion is worse than none.
	 */
	int limit = (int)(wl / 3);
	if (limit < 1)
		limit = 1;
	if (limit > 3)
		limit = 3;
	return best_d <= limit ? best : NULL;
}
