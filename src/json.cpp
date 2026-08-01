/**
 * @file json.cpp
 *
 * Minimal JSON reader and writer. See json.h.
 */
#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Writer                                                             */
/* ------------------------------------------------------------------ */

struct JsonWriter {
	char *buf;
	size_t len;
	size_t cap;
	int depth;
	int need_comma; /* a member has been written at this depth */
	int pending_gap;
	int inline_depth; /* >0 while inside an inline array */
};

static void jw_raw(JsonWriter *w, const char *s, size_t n)
{
	if (w->len + n + 1 > w->cap) {
		size_t cap = w->cap ? w->cap * 2 : 4096;
		while (cap < w->len + n + 1)
			cap *= 2;
		w->buf = (char *)realloc(w->buf, cap);
		w->cap = cap;
	}
	memcpy(w->buf + w->len, s, n);
	w->len += n;
	w->buf[w->len] = '\0';
}

static void jw_puts(JsonWriter *w, const char *s)
{
	jw_raw(w, s, strlen(s));
}

static void jw_indent(JsonWriter *w)
{
	for (int i = 0; i < w->depth; i++)
		jw_puts(w, "  ");
}

/** Emit the comma/newline separating this member from the previous one. */
static void jw_sep(JsonWriter *w)
{
	if (w->inline_depth > 0) {
		if (w->need_comma)
			jw_puts(w, ", ");
		w->need_comma = 1;
		return;
	}
	if (w->need_comma)
		jw_puts(w, ",");
	if (w->len > 0)
		jw_puts(w, "\n");
	if (w->pending_gap) {
		jw_puts(w, "\n");
		w->pending_gap = 0;
	}
	jw_indent(w);
	w->need_comma = 1;
}

static void jw_escaped(JsonWriter *w, const char *s)
{
	jw_puts(w, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		char tmp[8];
		switch (*p) {
		case '"':
			jw_puts(w, "\\\"");
			break;
		case '\\':
			jw_puts(w, "\\\\");
			break;
		case '\n':
			jw_puts(w, "\\n");
			break;
		case '\r':
			jw_puts(w, "\\r");
			break;
		case '\t':
			jw_puts(w, "\\t");
			break;
		default:
			if (*p < 0x20 || *p > 0x7E) {
				/* Character names are ASCII in practice; anything else is
				 * escaped so the document stays valid. */
				snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
				jw_puts(w, tmp);
			} else {
				tmp[0] = (char)*p;
				tmp[1] = '\0';
				jw_puts(w, tmp);
			}
		}
	}
	jw_puts(w, "\"");
}

static void jw_key(JsonWriter *w, const char *key)
{
	jw_sep(w);
	if (key != NULL) {
		jw_escaped(w, key);
		jw_puts(w, ": ");
	}
}

JsonWriter *jw_new(void)
{
	JsonWriter *w = (JsonWriter *)calloc(1, sizeof(*w));
	return w;
}

void jw_free(JsonWriter *w)
{
	if (w == NULL)
		return;
	free(w->buf);
	free(w);
}

const char *jw_text(const JsonWriter *w)
{
	return w->buf != NULL ? w->buf : "";
}

size_t jw_len(const JsonWriter *w)
{
	return w->len;
}

void jw_obj_open(JsonWriter *w, const char *key)
{
	jw_key(w, key);
	jw_puts(w, "{");
	w->depth++;
	w->need_comma = 0;
}

void jw_obj_close(JsonWriter *w)
{
	w->depth--;
	jw_puts(w, "\n");
	jw_indent(w);
	jw_puts(w, "}");
	w->need_comma = 1;
	w->pending_gap = 0;
}

void jw_arr_open(JsonWriter *w, const char *key)
{
	jw_key(w, key);
	jw_puts(w, "[");
	w->depth++;
	w->need_comma = 0;
}

void jw_arr_close(JsonWriter *w)
{
	w->depth--;
	if (w->need_comma) {
		jw_puts(w, "\n");
		jw_indent(w);
	}
	jw_puts(w, "]");
	w->need_comma = 1;
	w->pending_gap = 0;
}

void jw_arr_open_inline(JsonWriter *w, const char *key)
{
	jw_key(w, key);
	jw_puts(w, "[");
	w->inline_depth++;
	w->need_comma = 0;
}

void jw_arr_close_inline(JsonWriter *w)
{
	w->inline_depth--;
	jw_puts(w, "]");
	w->need_comma = 1;
	w->pending_gap = 0;
}

void jw_int(JsonWriter *w, const char *key, long long v)
{
	char tmp[32];
	jw_key(w, key);
	snprintf(tmp, sizeof(tmp), "%lld", v);
	jw_puts(w, tmp);
}

void jw_bool(JsonWriter *w, const char *key, int v)
{
	jw_key(w, key);
	jw_puts(w, v ? "true" : "false");
}

void jw_str(JsonWriter *w, const char *key, const char *v)
{
	jw_key(w, key);
	jw_escaped(w, v);
}

void jw_bool_or_int(JsonWriter *w, const char *key, long long v)
{
	if (v == 0 || v == 1)
		jw_bool(w, key, (int)v);
	else
		jw_int(w, key, v);
}

void jw_null(JsonWriter *w, const char *key)
{
	jw_key(w, key);
	jw_puts(w, "null");
}

void jw_gap(JsonWriter *w)
{
	w->pending_gap = 1;
}

/* ------------------------------------------------------------------ */
/* Reader                                                             */
/* ------------------------------------------------------------------ */

struct JValue {
	JType type;
	int b;
	long long i;
	char *s;
	JValue **items;
	char **keys;
	int n;
};

typedef struct Parser {
	const char *p;
	const char *start;
	char *err;
	int failed;
} Parser;

static void perr(Parser *ps, const char *fmt, ...)
{
	if (ps->failed)
		return;
	ps->failed = 1;
	if (ps->err == NULL)
		return;

	/* Report a line and column; a bare byte offset is useless when hand-editing. */
	int line = 1, col = 1;
	for (const char *q = ps->start; q < ps->p; q++) {
		if (*q == '\n') {
			line++;
			col = 1;
		} else {
			col++;
		}
	}

	char msg[JSON_ERR_LEN];
	va_list va;
	va_start(va, fmt);
	vsnprintf(msg, sizeof(msg), fmt, va);
	va_end(va);
	snprintf(ps->err, JSON_ERR_LEN, "line %d, column %d: %s", line, col, msg);
}

static JValue *jv_new(JType t)
{
	JValue *v = (JValue *)calloc(1, sizeof(*v));
	v->type = t;
	return v;
}

void jv_free(JValue *v)
{
	if (v == NULL)
		return;
	for (int i = 0; i < v->n; i++) {
		jv_free(v->items[i]);
		if (v->keys != NULL)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->s);
	free(v);
}

static void skip_ws(Parser *ps)
{
	for (;;) {
		while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
			ps->p++;
		/*
		 * JSON has no comments, but a hand-edited character file benefits from
		 * them enormously, so line and block comments are accepted on input.
		 * They are never emitted, and strict JSON tools will reject them.
		 */
		if (ps->p[0] == '/' && ps->p[1] == '/') {
			while (*ps->p && *ps->p != '\n')
				ps->p++;
			continue;
		}
		if (ps->p[0] == '/' && ps->p[1] == '*') {
			ps->p += 2;
			while (*ps->p && !(ps->p[0] == '*' && ps->p[1] == '/'))
				ps->p++;
			if (*ps->p)
				ps->p += 2;
			continue;
		}
		return;
	}
}

static JValue *parse_value(Parser *ps);

static char *parse_string_raw(Parser *ps)
{
	if (*ps->p != '"') {
		perr(ps, "expected a string");
		return NULL;
	}
	ps->p++;

	size_t cap = 32, len = 0;
	char *out = (char *)malloc(cap);

	while (*ps->p != '"') {
		if (*ps->p == '\0') {
			perr(ps, "unterminated string");
			free(out);
			return NULL;
		}
		unsigned char c = (unsigned char)*ps->p++;
		unsigned char emit;

		if (c == '\\') {
			char e = *ps->p++;
			switch (e) {
			case '"':
				emit = '"';
				break;
			case '\\':
				emit = '\\';
				break;
			case '/':
				emit = '/';
				break;
			case 'n':
				emit = '\n';
				break;
			case 'r':
				emit = '\r';
				break;
			case 't':
				emit = '\t';
				break;
			case 'b':
				emit = '\b';
				break;
			case 'f':
				emit = '\f';
				break;
			case 'u': {
				unsigned v = 0;
				for (int k = 0; k < 4; k++) {
					char h = *ps->p++;
					v <<= 4;
					if (h >= '0' && h <= '9')
						v |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f')
						v |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F')
						v |= (unsigned)(h - 'A' + 10);
					else {
						perr(ps, "bad \\u escape");
						free(out);
						return NULL;
					}
				}
				if (v > 0xFF) {
					perr(ps, "\\u%04x is outside the byte range a character "
					         "name can hold",
					    v);
					free(out);
					return NULL;
				}
				emit = (unsigned char)v;
				break;
			}
			default:
				perr(ps, "unknown escape \\%c", e);
				free(out);
				return NULL;
			}
		} else {
			emit = c;
		}

		if (len + 2 > cap) {
			cap *= 2;
			out = (char *)realloc(out, cap);
		}
		out[len++] = (char)emit;
	}
	ps->p++; /* closing quote */
	out[len] = '\0';
	return out;
}

static void push(JValue *parent, char *key, JValue *child)
{
	int n = parent->n + 1;
	parent->items = (JValue **)realloc(parent->items, (size_t)n * sizeof(JValue *));
	parent->items[n - 1] = child;
	if (parent->type == JOBJ) {
		parent->keys = (char **)realloc(parent->keys, (size_t)n * sizeof(char *));
		parent->keys[n - 1] = key;
	}
	parent->n = n;
}

static JValue *parse_object(Parser *ps)
{
	JValue *v = jv_new(JOBJ);
	ps->p++; /* { */
	skip_ws(ps);
	if (*ps->p == '}') {
		ps->p++;
		return v;
	}
	for (;;) {
		skip_ws(ps);
		char *key = parse_string_raw(ps);
		if (ps->failed) {
			free(key);
			jv_free(v);
			return NULL;
		}
		skip_ws(ps);
		if (*ps->p != ':') {
			perr(ps, "expected ':' after key \"%s\"", key);
			free(key);
			jv_free(v);
			return NULL;
		}
		ps->p++;
		skip_ws(ps);
		JValue *child = parse_value(ps);
		if (ps->failed) {
			free(key);
			jv_free(child);
			jv_free(v);
			return NULL;
		}
		push(v, key, child);

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			skip_ws(ps);
			/* Trailing comma before '}' is tolerated; hand-editing invites it. */
			if (*ps->p == '}') {
				ps->p++;
				return v;
			}
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			return v;
		}
		perr(ps, "expected ',' or '}' in object");
		jv_free(v);
		return NULL;
	}
}

static JValue *parse_array(Parser *ps)
{
	JValue *v = jv_new(JARR);
	ps->p++; /* [ */
	skip_ws(ps);
	if (*ps->p == ']') {
		ps->p++;
		return v;
	}
	for (;;) {
		skip_ws(ps);
		JValue *child = parse_value(ps);
		if (ps->failed) {
			jv_free(child);
			jv_free(v);
			return NULL;
		}
		push(v, NULL, child);

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			skip_ws(ps);
			if (*ps->p == ']') {
				ps->p++;
				return v;
			}
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			return v;
		}
		perr(ps, "expected ',' or ']' in array");
		jv_free(v);
		return NULL;
	}
}

static JValue *parse_value(Parser *ps)
{
	skip_ws(ps);

	if (*ps->p == '{')
		return parse_object(ps);
	if (*ps->p == '[')
		return parse_array(ps);
	if (*ps->p == '"') {
		char *s = parse_string_raw(ps);
		if (ps->failed)
			return NULL;
		JValue *v = jv_new(JSTR);
		v->s = s;
		return v;
	}
	if (strncmp(ps->p, "true", 4) == 0) {
		ps->p += 4;
		JValue *v = jv_new(JBOOL);
		v->b = 1;
		return v;
	}
	if (strncmp(ps->p, "false", 5) == 0) {
		ps->p += 5;
		JValue *v = jv_new(JBOOL);
		v->b = 0;
		return v;
	}
	if (strncmp(ps->p, "null", 4) == 0) {
		ps->p += 4;
		return jv_new(JNULL);
	}

	if (*ps->p == '-' || isdigit((unsigned char)*ps->p)) {
		char *end = NULL;
		long long n = strtoll(ps->p, &end, 10);
		if (end == ps->p) {
			perr(ps, "malformed number");
			return NULL;
		}
		if (*end == '.' || *end == 'e' || *end == 'E') {
			perr(ps, "expected a whole number; every field in a character is "
			         "an integer");
			return NULL;
		}
		ps->p = end;
		JValue *v = jv_new(JINT);
		v->i = n;
		return v;
	}

	perr(ps, "unexpected character '%c'", *ps->p ? *ps->p : '?');
	return NULL;
}

JValue *json_parse(const char *text, char *err)
{
	Parser ps;
	ps.p = text;
	ps.start = text;
	ps.err = err;
	ps.failed = 0;

	JValue *v = parse_value(&ps);
	if (ps.failed) {
		jv_free(v);
		return NULL;
	}
	skip_ws(&ps);
	if (*ps.p != '\0') {
		perr(&ps, "unexpected trailing content");
		jv_free(v);
		return NULL;
	}
	return v;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

JType jv_type(const JValue *v)
{
	return v != NULL ? v->type : JNULL;
}

int jv_count(const JValue *v)
{
	return v != NULL ? v->n : 0;
}

const JValue *jv_at(const JValue *v, int i)
{
	if (v == NULL || i < 0 || i >= v->n)
		return NULL;
	return v->items[i];
}

const char *jv_key_at(const JValue *v, int i)
{
	if (v == NULL || v->type != JOBJ || i < 0 || i >= v->n)
		return NULL;
	return v->keys[i];
}

const JValue *jv_member_at(const JValue *v, int i)
{
	return jv_at(v, i);
}

const JValue *jv_get(const JValue *v, const char *key)
{
	if (v == NULL || v->type != JOBJ)
		return NULL;
	for (int i = 0; i < v->n; i++)
		if (strcmp(v->keys[i], key) == 0)
			return v->items[i];
	return NULL;
}

long long jv_int(const JValue *v, long long fallback)
{
	if (v == NULL)
		return fallback;
	if (v->type == JINT)
		return v->i;
	if (v->type == JBOOL)
		return v->b;
	return fallback;
}

int jv_bool(const JValue *v, int fallback)
{
	if (v == NULL)
		return fallback;
	if (v->type == JBOOL)
		return v->b;
	if (v->type == JINT)
		return v->i != 0;
	return fallback;
}

const char *jv_str(const JValue *v, const char *fallback)
{
	if (v == NULL || v->type != JSTR)
		return fallback;
	return v->s;
}

long long jv_get_int(const JValue *v, const char *key, long long fallback)
{
	return jv_int(jv_get(v, key), fallback);
}

int jv_get_bool(const JValue *v, const char *key, int fallback)
{
	return jv_bool(jv_get(v, key), fallback);
}

const char *jv_get_str(const JValue *v, const char *key, const char *fallback)
{
	return jv_str(jv_get(v, key), fallback);
}
