#pragma once
#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ASSERT(x)                                                              \
  do {                                                                         \
    if (!(x)) {                                                                \
      abort();                                                                 \
    }                                                                          \
  } while (0)

#define MUST_USE __attribute__((warn_unused_result))
typedef struct {
  uint8_t *data;
  uint64_t len;
} Slice;

MUST_USE static bool slice_is_empty(Slice s) {
  if (s.len == 0) {
    return true;
  }

  ASSERT(s.data != NULL);
  return false;
}

MUST_USE static Slice S(char *s) {
  const uint64_t s_len = strlen(s);
  const Slice slice = {.data = (uint8_t *)s, .len = s_len};
  return slice;
}

MUST_USE static Slice slice_trim_left(Slice s, uint8_t c) {
  Slice res = s;

  for (uint64_t s_i = 0; s_i < s.len; s_i++) {
    ASSERT(s.data != NULL);
    if (s.data[s_i] != c) {
      return res;
    }

    res.data += 1;
    res.len -= 1;
  }
  return res;
}

MUST_USE static Slice slice_trim_right(Slice s, uint8_t c) {
  Slice res = s;

  for (int64_t s_i = s.len - 1; s_i >= 0; s_i--) {
    ASSERT(s.data != NULL);
    if (s.data[s_i] != c) {
      return res;
    }

    res.len -= 1;
  }
  return res;
}

MUST_USE static Slice slice_trim(Slice s, uint8_t c) {
  Slice res = slice_trim_left(s, c);
  res = slice_trim_right(res, c);

  return res;
}

typedef struct {
  Slice slice;
  uint8_t sep;
} SplitIterator;

typedef struct {
  Slice slice;
  bool ok;
} SplitResult;

MUST_USE static SplitIterator slice_split_it(Slice slice, uint8_t sep) {
  return (SplitIterator){.slice = slice, .sep = sep};
}

MUST_USE static int64_t slice_indexof_byte(Slice haystack, uint8_t needle) {
  if (slice_is_empty(haystack)) {
    return -1;
  }

  const uint8_t *res = memchr(haystack.data, needle, haystack.len);
  if (res == NULL) {
    return -1;
  }

  return res - haystack.data;
}

MUST_USE static Slice slice_range(Slice src, uint64_t start, uint64_t end) {
  const uint64_t real_end = end == 0 ? src.len : end;
  ASSERT(start <= real_end);
  ASSERT(start <= src.len);
  ASSERT(real_end <= src.len);

  Slice res = {.data = src.data + start, .len = real_end - start};
  return res;
}

MUST_USE static SplitResult slice_split_next(SplitIterator *it) {
  if (slice_is_empty(it->slice)) {
    return (SplitResult){};
  }

  for (uint64_t _i = 0; _i < it->slice.len; _i++) {
    const int64_t idx = slice_indexof_byte(it->slice, it->sep);
    if (-1 == idx) {
      // Last element.
      SplitResult res = {.slice = it->slice, .ok = true};
      it->slice = (Slice){0};
      return res;
    }

    if (idx == 0) { // Multiple continguous separators.
      it->slice = slice_range(it->slice, idx + 1, 0);
      continue;
    } else {
      SplitResult res = {.slice = slice_range(it->slice, 0, idx), .ok = true};
      it->slice = slice_range(it->slice, idx + 1, 0);

      return res;
    }
  }
  return (SplitResult){};
}

MUST_USE static bool slice_eq(Slice a, Slice b) {
  if (a.data == NULL && b.data == NULL && a.len == b.len) {
    return true;
  }
  if (a.data == NULL) {
    return false;
  }
  if (b.data == NULL) {
    return false;
  }

  if (a.len != b.len) {
    return false;
  }

  ASSERT(a.data != NULL);
  ASSERT(b.data != NULL);
  ASSERT(a.len == b.len);

  return memcmp(a.data, b.data, a.len) == 0;
}

MUST_USE static int64_t slice_indexof_slice(Slice haystack, Slice needle) {
  if (haystack.data == NULL) {
    return -1;
  }

  if (haystack.len == 0) {
    return -1;
  }

  if (needle.data == NULL) {
    return -1;
  }

  if (needle.len == 0) {
    return -1;
  }

  if (needle.len > haystack.len) {
    return -1;
  }

  uint64_t haystack_idx = 0;

  for (uint64_t _i = 0; _i < haystack.len; _i++) {
    ASSERT(haystack_idx < haystack.len);
    ASSERT(haystack_idx + needle.len <= haystack.len);

    const Slice to_search = slice_range(haystack, haystack_idx, 0);
    const int64_t found_idx = slice_indexof_byte(to_search, needle.data[0]);
    if (found_idx == -1) {
      return -1;
    }

    ASSERT(found_idx <= (int64_t)to_search.len);
    if (found_idx + needle.len > to_search.len) {
      return -1;
    }

    const Slice found_candidate =
        slice_range(to_search, found_idx, found_idx + needle.len);
    if (slice_eq(found_candidate, needle)) {
      return haystack_idx + found_idx;
    }
    haystack_idx += found_idx + 1;
  }

  return -1;
}

MUST_USE static uint64_t slice_count_u8(Slice s, uint8_t c) {
  uint64_t res = 0;
  for (uint64_t i = 0; i < s.len; i++) {
    res += (s.data[i] == c);
  }
  return res;
}

typedef struct {
  uint8_t *start;
  uint8_t *end;
} Arena;

MUST_USE static void *arena_alloc(Arena *a, uint64_t size, uint64_t align,
                                  uint64_t count) {
  ASSERT(a->start != NULL);

  const uint64_t padding = (int64_t)(-(uint64_t)a->start & (align - 1));
  ASSERT(padding <= align);

  const int64_t available = (uint64_t)a->end - (uint64_t)a->start - padding;
  if (available < 0 || count > available / size) {
    abort();
  }

  void *res = a->start + padding;
  ASSERT(res != NULL);
  ASSERT(res <= (void *)a->end);

  a->start += padding + count * size;
  ASSERT(a->start <= a->end);
  ASSERT((uint64_t)a->start % align == 0); // Aligned.

  return memset(res, 0, count * size);
}

static void dyn_grow(void *slice, uint64_t size, uint64_t align, Arena *a) {
  ASSERT(NULL != slice);

  struct {
    void *data;
    uint64_t len;
    uint64_t cap;
  } replica;

  memcpy(&replica, slice, sizeof(replica));

  if (NULL == replica.data) { // First allocation
    replica.cap = 1;
    replica.data = arena_alloc(a, 2 * size, align, replica.cap);
  } else if (a->start == replica.data + size * replica.cap) { // Optimization.
    // This is the case of growing the array which is at the end of the arena.
    // In that case we can simply bump the arena pointer and avoid any copies.
    (void)arena_alloc(a, size, 1, replica.cap);
  } else { // General case.
    void *data = arena_alloc(a, 2 * size, align, replica.cap);
    memcpy(data, replica.data, size * replica.len);
    replica.data = data;
  }
  replica.cap *= 2;

  ASSERT(NULL != slice);
  memcpy(slice, &replica, sizeof(replica));
}

typedef struct {
  uint8_t *data;
  uint64_t len, cap;
} DynArrayU8;

MUST_USE static Slice dyn_array_u8_range(DynArrayU8 src, uint64_t start,
                                         uint64_t end) {
  Slice src_slice = {.data = src.data, .len = src.len};
  return slice_range(src_slice, start, end);
}

#define dyn_push(s, arena)                                                     \
  ((s)->len >= (s)->cap                                                        \
   ? dyn_grow(s, sizeof(*(s)->data), _Alignof(*(s)->data), arena),             \
   (s)->data + (s)->len++ : (s)->data + (s)->len++)

#define dyn_pop(s)                                                             \
  do {                                                                         \
    ASSERT((s)->len > 0);                                                      \
    memset(&(s)->data[(s)->len - 1], 0, sizeof((s)->data[(s)->len - 1]));      \
    (s)->len -= 1;                                                             \
  } while (0)

#define dyn_last(s)                                                            \
  ((s)->len == 0 ? (((typeof((s)->data[0]) *(*)())0)())                        \
                 : (&((s)->data[(s)->len - 1])))

#define dyn_append_slice(dst, src, arena)                                      \
  do {                                                                         \
    for (uint64_t i = 0; i < src.len; i++) {                                   \
      *dyn_push(dst, arena) = src.data[i];                                     \
    }                                                                          \
  } while (0)

MUST_USE static Slice dyn_array_u8_to_slice(DynArrayU8 dyn) {
  return (Slice){.data = dyn.data, .len = dyn.len};
}

static void dyn_array_u8_append_cstr(DynArrayU8 *dyn, char *s, Arena *arena) {
  dyn_append_slice(dyn, S(s), arena);
}

static void dyn_array_u8_append_u64(DynArrayU8 *dyn, uint64_t n, Arena *arena) {
  uint8_t tmp[30] = {0};
  const int written_count = snprintf((char *)tmp, sizeof(tmp), "%lu", n);

  ASSERT(written_count > 0);

  Slice slice = {.data = tmp, .len = written_count};
  dyn_append_slice(dyn, slice, arena);
}

static void dyn_array_u8_append_u128_hex(DynArrayU8 *dyn, __uint128_t n,
                                         Arena *arena) {
  uint8_t tmp[32 + 1] = {0};
  snprintf((char *)&tmp[0], 16, "%lx", (uint64_t)(n << 8));
  snprintf((char *)&tmp[16], 16, "%lx", (uint64_t)(n & UINT64_MAX));

  Slice slice = {.data = tmp, .len = sizeof(tmp)};
  dyn_append_slice(dyn, slice, arena);
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

MUST_USE static Arena arena_make_from_virtual_mem(uint64_t size) {
  void *ptr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

  if (ptr == NULL) {
    fprintf(stderr, "failed to mmap: %d %s\n", errno, strerror(errno));
    exit(errno);
  }
  return (Arena){.start = ptr, .end = ptr + size};
}

typedef enum {
  LV_SLICE,
  LV_U64,
  LV_U128,
} LogValueKind;

typedef struct {
  LogValueKind kind;
  union {
    Slice s;
    uint64_t n64;
    __uint128_t n128;
  };
} LogValue;

typedef struct {
  Slice key;
  LogValue value;
} LogEntry;

MUST_USE static LogEntry LCI(char *k, uint64_t v) {
  return ((LogEntry){
      .key = S(k),
      .value.kind = LV_U64,
      .value.n64 = v,
  });
}

MUST_USE static LogEntry LCII(char *k, __uint128_t v) {
  return ((LogEntry){
      .key = S(k),
      .value.kind = LV_U128,
      .value.n128 = v,
  });
}

MUST_USE static LogEntry LCS(char *k, Slice v) {
  return ((LogEntry){
      .key = S(k),
      .value.kind = LV_SLICE,
      .value.s = v,
  });
}

#define LOG_ARGS_COUNT(...)                                                    \
  (sizeof((LogEntry[]){__VA_ARGS__}) / sizeof(LogEntry))
#define log(msg, tmp_arena, ...)                                               \
  do {                                                                         \
    Slice log_line = make_log_line(S(msg), &tmp_arena,                         \
                                   LOG_ARGS_COUNT(__VA_ARGS__), __VA_ARGS__);  \
    write(2, log_line.data, log_line.len);                                     \
  } while (0)

MUST_USE static Slice log_entry_quote_value(Slice entry, Arena *arena) {
  uint64_t quote_count = slice_count_u8(entry, '"');
  if (quote_count == 0) { // Optimization: do not quote anything.
    return entry;
  }

  DynArrayU8 sb = {0};
  *dyn_push(&sb, arena) = '"';

  for (uint64_t i = 0; i < entry.len; i++) {
    uint8_t c = entry.data[i];
    if ('"' != c) { // Easy case.
      *dyn_push(&sb, arena) = c;
      continue;
    }

    *dyn_push(&sb, arena) = '\\';
    *dyn_push(&sb, arena) = c;
  }
  *dyn_push(&sb, arena) = '"';

  return dyn_array_u8_to_slice(sb);
}

MUST_USE static Slice make_log_line(Slice msg, Arena *arena, int32_t args_count,
                                    ...) {
  DynArrayU8 sb = {0};
  dyn_append_slice(&sb, S("message="), arena);
  dyn_append_slice(&sb, msg, arena);
  dyn_append_slice(&sb, S(" "), arena);

  va_list argp = {0};
  va_start(argp, args_count);
  for (int32_t i = 0; i < args_count; i++) {
    LogEntry entry = va_arg(argp, LogEntry);

    dyn_append_slice(&sb, entry.key, arena);
    dyn_append_slice(&sb, S("="), arena);

    switch (entry.value.kind) {
    case LV_SLICE: {
      Slice value = log_entry_quote_value(entry.value.s, arena);
      dyn_append_slice(&sb, value, arena);
      break;
    }
    case LV_U64:
      dyn_array_u8_append_u64(&sb, entry.value.n64, arena);
      break;
    case LV_U128:
      dyn_array_u8_append_u128_hex(&sb, entry.value.n128, arena);
      break;
    default:
      ASSERT(0 && "invalid LogValueKind");
    }

    dyn_append_slice(&sb, S(" "), arena);
  }
  va_end(argp);

  ASSERT(' ' == *dyn_last(&sb));
  dyn_pop(&sb);
  dyn_append_slice(&sb, S("\n"), arena);

  return dyn_array_u8_to_slice(sb);
}
