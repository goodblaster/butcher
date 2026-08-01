/**
 * @file json.h
 *
 * A small JSON reader and writer.
 *
 * Hand-rolled because this subproject has no external dependencies and the
 * schema is known and shallow. Deliberately narrow: numbers must be integers,
 * because every field in a character is one, and rejecting "3.5" with a clear
 * message beats silently truncating it.
 */
#ifndef BUTCHER_JSON_H
#define BUTCHER_JSON_H

#include <stddef.h>

#define JSON_ERR_LEN 256

/* ------------------------------------------------------------------ */
/* Writer                                                             */
/* ------------------------------------------------------------------ */

typedef struct JsonWriter JsonWriter;

JsonWriter *jw_new(void);
void jw_free(JsonWriter *w);

/** Borrowed pointer to the accumulated text, NUL-terminated. */
const char *jw_text(const JsonWriter *w);
size_t jw_len(const JsonWriter *w);

void jw_obj_open(JsonWriter *w, const char *key);
void jw_obj_close(JsonWriter *w);
void jw_arr_open(JsonWriter *w, const char *key);
void jw_arr_close(JsonWriter *w);

/** Array whose elements stay on one line -- for short numeric rows. */
void jw_arr_open_inline(JsonWriter *w, const char *key);
void jw_arr_close_inline(JsonWriter *w);

void jw_int(JsonWriter *w, const char *key, long long v);
void jw_bool(JsonWriter *w, const char *key, int v);
void jw_str(JsonWriter *w, const char *key, const char *v);

/** true/false for 0 and 1, otherwise the integer -- exact for any byte. */
void jw_bool_or_int(JsonWriter *w, const char *key, long long v);

/** null literal. */
void jw_null(JsonWriter *w, const char *key);

/** Blank line before the next member; purely cosmetic grouping. */
void jw_gap(JsonWriter *w);

/* ------------------------------------------------------------------ */
/* Reader                                                             */
/* ------------------------------------------------------------------ */

typedef enum JType {
	JNULL,
	JBOOL,
	JINT,
	JSTR,
	JARR,
	JOBJ
} JType;

typedef struct JValue JValue;

/** Parse a document. Returns NULL with err set; free with jv_free. */
JValue *json_parse(const char *text, char *err);
void jv_free(JValue *v);

JType jv_type(const JValue *v);
int jv_count(const JValue *v);            /**< members or elements */
const JValue *jv_at(const JValue *v, int i);        /**< array element */
const char *jv_key_at(const JValue *v, int i);      /**< object member name */
const JValue *jv_member_at(const JValue *v, int i);

/** Object lookup; NULL when absent or when @p v is not an object. */
const JValue *jv_get(const JValue *v, const char *key);

long long jv_int(const JValue *v, long long fallback);
int jv_bool(const JValue *v, int fallback);
const char *jv_str(const JValue *v, const char *fallback);

/** Lookup helpers that apply a default when the key is missing. */
long long jv_get_int(const JValue *v, const char *key, long long fallback);
int jv_get_bool(const JValue *v, const char *key, int fallback);
const char *jv_get_str(const JValue *v, const char *key, const char *fallback);

#endif /* BUTCHER_JSON_H */
