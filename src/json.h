/*
 * json.h — strict RFC 8259 JSON codec for asngn-mcp.
 *
 * DOM-style value tree. In-house by design: JSON exists only in the MCP
 * layer; the rest of asngn speaks xCDN. Depends only on libc.
 *
 * Guarantees:
 *   - Strict parsing: UTF-8 only (validated), \uXXXX escapes with surrogate
 *     pairs decoded to UTF-8, depth cap 64, no comments, no trailing
 *     garbage, no NaN/Inf, no unescaped control characters in strings.
 *   - Numbers carry a double plus an int64 view when the literal is
 *     integral and in range (jx_is_int).
 *   - No global state; every function is thread-compatible over disjoint
 *     trees.
 *
 * Ownership:
 *   - Constructors return NULL on allocation failure (jx_string also on
 *     invalid UTF-8, jx_double also on non-finite input).
 *   - jx_array_push / jx_object_set ALWAYS take ownership of the value,
 *     including on failure (the value is freed); a NULL value yields -1.
 *     This makes chained construction leak-free: free the outermost value
 *     once and check the accumulated status.
 *   - Object keys are copied and treated as NUL-terminated C strings.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_JSON_H
#define ASNGN_JSON_H

#include <stddef.h>

typedef enum {
  JX_NULL = 0,
  JX_BOOL,
  JX_NUMBER,
  JX_STRING,
  JX_ARRAY,
  JX_OBJECT
} jx_type;

typedef struct jx_value jx_value;

/* Parse [text, text+len) as exactly one JSON text (any value type at the
 * top level). Returns 0 and sets *out on success; nonzero on any error
 * with *out = NULL. text need not be NUL-terminated. */
int  jx_parse(const char *text, size_t len, jx_value **out);
void jx_free(jx_value *v); /* NULL is a no-op */

/* Deep copy; NULL on allocation failure or v == NULL. */
jx_value *jx_clone(const jx_value *v);

/* ---- constructors ------------------------------------------------------- */

jx_value *jx_null(void);
jx_value *jx_bool(int b);
jx_value *jx_int(long long i);
jx_value *jx_double(double d);        /* NULL on NaN/Inf */
jx_value *jx_string(const char *utf8); /* copies; NULL on invalid UTF-8 */
jx_value *jx_array(void);
jx_value *jx_object(void);

/* ---- accessors ---------------------------------------------------------- */

jx_type jx_typeof(const jx_value *v); /* JX_NULL when v == NULL */

int         jx_bool_value(const jx_value *v);    /* 0 unless JX_BOOL */
int         jx_is_int(const jx_value *v);
long long   jx_int_value(const jx_value *v);     /* truncates non-ints */
double      jx_double_value(const jx_value *v);
const char *jx_string_value(const jx_value *v);  /* NUL-terminated; NULL
                                                    unless JX_STRING */
size_t      jx_string_length(const jx_value *v); /* bytes, embedded NULs
                                                    included */

/* ---- containers --------------------------------------------------------- */

int       jx_array_push(jx_value *arr, jx_value *v); /* 0 ok; owns v */
size_t    jx_array_len(const jx_value *arr);
jx_value *jx_array_at(const jx_value *arr, size_t i); /* borrowed */

int       jx_object_set(jx_value *obj, const char *key, jx_value *v);
jx_value *jx_object_get(const jx_value *obj, const char *key); /* borrowed */
size_t    jx_object_count(const jx_value *obj);
const char *jx_object_key_at(const jx_value *obj, size_t i);
jx_value   *jx_object_value_at(const jx_value *obj, size_t i);

/* ---- writer ------------------------------------------------------------- */

/* Serialize to a malloc'd NUL-terminated string; NULL on allocation
 * failure. pretty == 0 emits compact JSON with no raw newlines (control
 * characters in strings are escaped), suitable for line-delimited
 * transports. */
char *jx_write(const jx_value *v, int pretty);

#endif /* ASNGN_JSON_H */
