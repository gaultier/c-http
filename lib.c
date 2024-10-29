#pragma once
#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#define KiB (1024ULL)
#define MiB (1024ULL * Ki)
#define GiB (1024ULL * Mi)
#define TiB (1024ULL * Gi)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef __uint128_t u128;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define static_array_len(a) (sizeof(a) / sizeof((a)[0]))

#define CLAMP(n, min, max)                                                     \
  do {                                                                         \
    if (*(n) < (min)) {                                                        \
      *(n) = (min);                                                            \
    }                                                                          \
    if (*(n) > (max)) {                                                        \
      *(n) = (max);                                                            \
    }                                                                          \
  } while (0)

static void print_stacktrace(const char *file, int line, const char *function) {
  fprintf(stderr, "%s:%d:%s\n", file, line, function);
  // TODO
}

#define ASSERT(x)                                                              \
  (x) ? (0)                                                                    \
      : (print_stacktrace(__FILE__, __LINE__, __FUNCTION__), __builtin_trap(), \
         0)

#define AT_PTR(arr, len, idx)                                                  \
  (((i64)(idx) >= (i64)(len)) ? (__builtin_trap(), &(arr)[0])                  \
                              : (ASSERT(nullptr != arr), (&(arr)[idx])))

#define AT(arr, len, idx) (*AT_PTR(arr, len, idx))

#define slice_at(s, idx) (AT((s).data, (s).len, idx))

typedef u32 Error;

[[nodiscard]] static bool ch_is_hex_digit(u8 c) {
  return ('0' <= c && c <= '9') || ('A' <= c && c <= 'F') ||
         ('a' <= c && c <= 'f');
}

[[nodiscard]] static u8 ch_from_hex(u8 c) {
  ASSERT(ch_is_hex_digit(c));

  if ('0' <= c && c <= '9') {
    return c - '0';
  }

  if ('A' <= c && c <= 'F') {
    return 10 + c - 'A';
  }

  if ('a' <= c && c <= 'f') {
    return 10 + c - 'a';
  }

  ASSERT(false);
}

typedef struct {
  u8 *data;
  u64 len;
} String;

#define slice_is_empty(s)                                                      \
  (((s).len == 0) ? true : (ASSERT(nullptr != (s).data), false))

#define S(s) ((String){.data = (u8 *)s, .len = sizeof(s) - 1})

[[nodiscard]] static String string_trim_left(String s, u8 c) {
  String res = s;

  for (u64 s_i = 0; s_i < s.len; s_i++) {
    ASSERT(s.data != nullptr);
    if (AT(s.data, s.len, s_i) != c) {
      return res;
    }

    res.data += 1;
    res.len -= 1;
  }
  return res;
}

[[nodiscard]] static String string_trim_right(String s, u8 c) {
  String res = s;

  for (i64 s_i = (i64)s.len - 1; s_i >= 0; s_i--) {
    ASSERT(s.data != nullptr);
    if (AT(s.data, s.len, s_i) != c) {
      return res;
    }

    res.len -= 1;
  }
  return res;
}

[[nodiscard]] static String string_trim(String s, u8 c) {
  String res = string_trim_left(s, c);
  res = string_trim_right(res, c);

  return res;
}

typedef struct {
  String s;
  u8 sep;
} SplitIterator;

typedef struct {
  String s;
  bool ok;
} SplitResult;

[[nodiscard]] static SplitIterator string_split(String s, u8 sep) {
  return (SplitIterator){.s = s, .sep = sep};
}

[[nodiscard]] static i64 string_indexof_byte(String haystack, u8 needle) {
  if (slice_is_empty(haystack)) {
    return -1;
  }

  const u8 *res = memchr(haystack.data, needle, haystack.len);
  if (res == nullptr) {
    return -1;
  }

  return res - haystack.data;
}

#define slice_range(s, start, end)                                             \
  (ASSERT((start) <= (end == 0 ? (s).len : end)), ASSERT((start) <= (s).len),  \
   ASSERT((end == 0 ? (s).len : end) <= (s).len),                              \
   (0 == (s).len) ? s                                                          \
                  : (typeof((s))){                                             \
                        .data = &(s).data[start],                              \
                        .len = (end == 0 ? (s).len : end) - (start),           \
                    })

[[nodiscard]] static SplitResult string_split_next(SplitIterator *it) {
  if (slice_is_empty(it->s)) {
    return (SplitResult){0};
  }

  for (u64 _i = 0; _i < it->s.len; _i++) {
    const i64 idx = string_indexof_byte(it->s, it->sep);
    if (-1 == idx) {
      // Last element.
      SplitResult res = {.s = it->s, .ok = true};
      it->s = (String){0};
      return res;
    }

    if (idx == 0) { // Multiple contiguous separators.
      it->s = slice_range(it->s, (u64)idx + 1, 0);
      continue;
    } else {
      SplitResult res = {.s = slice_range(it->s, 0, (u64)idx), .ok = true};
      it->s = slice_range(it->s, (u64)idx + 1, 0);

      return res;
    }
  }
  return (SplitResult){0};
}

[[nodiscard]] static bool string_eq(String a, String b) {
  if (a.len == 0 && b.len == 0) {
    return true;
  }

  if (a.data == nullptr && b.data == nullptr && a.len == b.len) {
    return true;
  }
  if (a.data == nullptr) {
    return false;
  }
  if (b.data == nullptr) {
    return false;
  }

  if (a.len != b.len) {
    return false;
  }

  ASSERT(a.data != nullptr);
  ASSERT(b.data != nullptr);
  ASSERT(a.len == b.len);

  return memcmp(a.data, b.data, a.len) == 0;
}

[[nodiscard]] static i64 string_indexof_string(String haystack, String needle) {
  if (haystack.data == nullptr) {
    return -1;
  }

  if (haystack.len == 0) {
    return -1;
  }

  if (needle.data == nullptr) {
    return -1;
  }

  if (needle.len == 0) {
    return -1;
  }

  if (needle.len > haystack.len) {
    return -1;
  }

  void *ptr = memmem(haystack.data, haystack.len, needle.data, needle.len);
  if (nullptr == ptr) {
    return -1;
  }

  u64 res = (u64)((u8 *)ptr - haystack.data);
  ASSERT(res < haystack.len);
  return (i64)res;
}

[[nodiscard]] static bool string_starts_with(String haystack, String needle) {
  if (haystack.len == 0 || haystack.len < needle.len) {
    return false;
  }
  ASSERT(nullptr != haystack.data);
  ASSERT(nullptr != needle.data);

  String start = slice_range(haystack, 0, needle.len);

  return string_eq(needle, start);
}

[[maybe_unused]] [[nodiscard]] static bool string_ends_with(String haystack,
                                                            String needle) {
  if (haystack.len == 0 || haystack.len < needle.len) {
    return false;
  }
  ASSERT(nullptr != haystack.data);
  ASSERT(nullptr != needle.data);

  String end = slice_range(haystack, haystack.len - needle.len, 0);

  return string_eq(needle, end);
}

typedef struct {
  u64 n;
  bool err;
  bool present;
} ParseNumberResult;

[[nodiscard]] static ParseNumberResult string_parse_u64_decimal(String s) {
  String trimmed = string_trim(s, ' ');

  ParseNumberResult res = {0};

  for (u64 i = 0; i < trimmed.len; i++) {
    u8 c = slice_at(trimmed, i);

    if (!('0' <= c && c <= '9')) { // Error.
      res.err = true;
      return res;
    }

    res.n *= 10;
    res.n += (u8)slice_at(trimmed, i) - '0';
  }
  res.present = true;
  return res;
}

typedef struct {
  u8 *start;
  u8 *end;
} Arena;

__attribute((malloc, alloc_size(2, 4), alloc_align(3)))
[[nodiscard]] static void *
arena_alloc(Arena *a, u64 size, u64 align, u64 count) {
  ASSERT(a->start != nullptr);

  const u64 padding = (-(u64)a->start & (align - 1));
  ASSERT(padding <= align);

  const i64 available = (i64)a->end - (i64)a->start - (i64)padding;
  ASSERT(available >= 0);
  ASSERT(count <= (u64)available / size);

  void *res = a->start + padding;
  ASSERT(res != nullptr);
  ASSERT(res <= (void *)a->end);

  a->start += padding + count * size;
  ASSERT(a->start <= a->end);
  ASSERT((u64)a->start % align == 0); // Aligned.

  return memset(res, 0, count * size);
}

[[nodiscard]] static char *string_to_cstr(String s, Arena *arena) {
  char *res = arena_alloc(arena, 1, 1, s.len + 1);
  if (NULL != s.data) {
    memcpy(res, s.data, s.len);
  }

  ASSERT(0 == AT(res, s.len + 1, s.len));

  return res;
}

static void dyn_grow(void *slice, u64 size, u64 align, u64 count, Arena *a) {
  ASSERT(nullptr != slice);

  struct {
    void *data;
    u64 len;
    u64 cap;
  } replica;

  memcpy(&replica, slice, sizeof(replica));
  ASSERT(replica.cap < count);

  u64 new_cap = replica.cap == 0 ? 2 : replica.cap;
  for (u64 i = 0; i < 64; i++) {
    if (new_cap < count) {
      ASSERT(new_cap < UINT64_MAX / 2);
      ASSERT(false == ckd_mul(&new_cap, new_cap, 2));
    } else {
      break;
    }
  }
  ASSERT(new_cap >= 2);
  ASSERT(new_cap >= count);
  ASSERT(new_cap > replica.cap);

  u64 array_end = 0;
  u64 array_bytes_count = 0;
  ASSERT(false == ckd_mul(&array_bytes_count, size, replica.cap));
  ASSERT(false == ckd_add(&array_end, (u64)replica.data, array_bytes_count));
  ASSERT((u64)replica.data <= array_end);
  ASSERT(array_end < (u64)a->end);

  if (nullptr ==
      replica.data) { // First allocation ever for this dynamic array.
    replica.data = arena_alloc(a, size, align, new_cap);
  } else if ((u64)a->start == array_end) { // Optimization.
    // This is the case of growing the array which is at the end of the arena.
    // In that case we can simply bump the arena pointer and avoid any copies.
    (void)arena_alloc(a, size, 1 /* Force no padding */, new_cap - replica.cap);
  } else { // General case.
    void *data = arena_alloc(a, size, align, new_cap);

    // Import check to avoid overlapping memory ranges in memcpy.
    ASSERT(data != replica.data);

    memcpy(data, replica.data, array_bytes_count);
    replica.data = data;
  }
  replica.cap = new_cap;

  ASSERT(nullptr != slice);
  memcpy(slice, &replica, sizeof(replica));
}

#define dyn_ensure_cap(dyn, new_cap, arena)                                    \
  ((dyn)->cap < (new_cap))                                                     \
      ? dyn_grow(dyn, sizeof(*(dyn)->data), _Alignof((dyn)->data[0]), new_cap, \
                 arena),                                                       \
      0 : 0

typedef struct {
  u8 *data;
  u64 len, cap;
} DynU8;

typedef struct {
  String *data;
  u64 len, cap;
} DynString;

#define dyn_push(s, arena)                                                     \
  (dyn_ensure_cap(s, (s)->len + 1, arena), (s)->data + (s)->len++)

#define dyn_pop(s)                                                             \
  do {                                                                         \
    ASSERT((s)->len > 0);                                                      \
    memset(dyn_last_ptr(s), 0, sizeof((s)->data[(s)->len - 1]));               \
    (s)->len -= 1;                                                             \
  } while (0)

#define dyn_last_ptr(s) AT_PTR((s)->data, (s)->len, (s)->len - 1)

#define dyn_at_ptr(s, idx) AT_PTR((s)->data, (s)->len, idx)

#define dyn_at(s, idx) AT((s).data, (s).len, idx)

#define dyn_append_slice(dst, src, arena)                                      \
  do {                                                                         \
    dyn_ensure_cap(dst, (dst)->len + (src).len, arena);                        \
    for (u64 _iii = 0; _iii < src.len; _iii++) {                               \
      *dyn_push(dst, arena) = slice_at(src, _iii);                             \
    }                                                                          \
  } while (0)

#define dyn_slice(T, dyn) ((T){.data = dyn.data, .len = dyn.len})

static void dynu8_append_u64(DynU8 *dyn, u64 n, Arena *arena) {
  u8 tmp[30] = {0};
  const int written_count = snprintf((char *)tmp, sizeof(tmp), "%lu", n);

  ASSERT(written_count > 0);

  String s = {.data = tmp, .len = (u64)written_count};
  dyn_append_slice(dyn, s, arena);
}

[[nodiscard]] static u8 u8_to_ch_hex(u8 n) {
  ASSERT(n < 16);

  if (n <= 9) {
    return n + '0';
  } else if (10 == n) {
    return 'a';
  } else if (11 == n) {
    return 'b';
  } else if (12 == n) {
    return 'c';
  } else if (13 == n) {
    return 'd';
  } else if (14 == n) {
    return 'e';
  } else if (15 == n) {
    return 'f';
  }
  ASSERT(0);
}

static void dynu8_append_u128_hex(DynU8 *dyn, u128 n, Arena *arena) {
  dyn_ensure_cap(dyn, dyn->len + 32, arena);
  u64 dyn_original_len = dyn->len;

  u8 it[16] = {0};
  ASSERT(sizeof(it) == sizeof(n));
  memcpy(it, (u8 *)&n, sizeof(n));

  for (u64 i = 0; i < sizeof(it); i++) {
    u8 c1 = it[i] % 16;
    u8 c2 = it[i] / 16;
    *dyn_push(dyn, arena) = u8_to_ch_hex(c2);
    *dyn_push(dyn, arena) = u8_to_ch_hex(c1);
  }
  ASSERT(32 == (dyn->len - dyn_original_len));
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

[[nodiscard]] static u64 round_up_multiple_of(u64 n, u64 multiple) {
  ASSERT(0 != multiple);

  if (0 == n % multiple) {
    return n; // No-op.
  }

  u64 factor = n / multiple;
  return (factor + 1) * n;
}

[[nodiscard]] static Arena arena_make_from_virtual_mem(u64 size) {
  u64 page_size = (u64)sysconf(_SC_PAGE_SIZE); // FIXME
  u64 alloc_real_size = round_up_multiple_of(size, page_size);
  ASSERT(0 == alloc_real_size % page_size);

  u64 mmap_size = alloc_real_size;
  // Page guard before.
  ASSERT(false == ckd_add(&mmap_size, mmap_size, page_size));
  // Page guard after.
  ASSERT(false == ckd_add(&mmap_size, mmap_size, page_size));

  u8 *alloc = mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
  ASSERT(nullptr != alloc);

  u64 page_guard_before = (u64)alloc;

  ASSERT(false == ckd_add((u64 *)&alloc, (u64)alloc, page_size));
  ASSERT(page_guard_before + page_size == (u64)alloc);

  u64 page_guard_after = (u64)0;
  ASSERT(false == ckd_add(&page_guard_after, (u64)alloc, alloc_real_size));
  ASSERT((u64)alloc + alloc_real_size == page_guard_after);
  ASSERT(page_guard_before + page_size + alloc_real_size == page_guard_after);

  ASSERT(0 == mprotect((void *)page_guard_before, page_size, PROT_NONE));
  ASSERT(0 == mprotect((void *)page_guard_after, page_size, PROT_NONE));

  // Trigger a page fault preemptively to detect invalid virtual memory
  // mappings.
  *(u8 *)alloc = 0;

  return (Arena){.start = alloc, .end = (u8 *)alloc + size};
}

typedef enum {
  LV_STRING,
  LV_U64,
} LogValueKind;

typedef enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_ERROR,
  LOG_LEVEL_FATAL,
} LogLevel;

typedef struct {
  LogValueKind kind;
  union {
    String s;
    u64 n64;
    u128 n128;
  };
} LogValue;

typedef struct {
  String key;
  LogValue value;
} LogEntry;

[[nodiscard]] static String log_level_to_string(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_DEBUG:
    return S("debug");
  case LOG_LEVEL_INFO:
    return S("info");
  case LOG_LEVEL_ERROR:
    return S("error");
  case LOG_LEVEL_FATAL:
    return S("fatal");
  default:
    ASSERT(false);
  }
}

[[nodiscard]] static LogEntry log_entry_int(String k, int v) {
  return (LogEntry){
      .key = k,
      .value.kind = LV_U64,
      .value.n64 = (u64)v,
  };
}

[[nodiscard]] static LogEntry log_entry_u16(String k, u16 v) {
  return (LogEntry){
      .key = k,
      .value.kind = LV_U64,
      .value.n64 = (u64)v,
  };
}

[[nodiscard]] static LogEntry log_entry_u32(String k, u32 v) {
  return (LogEntry){
      .key = k,
      .value.kind = LV_U64,
      .value.n64 = (u64)v,
  };
}

[[nodiscard]] static LogEntry log_entry_u64(String k, u64 v) {
  return (LogEntry){
      .key = k,
      .value.kind = LV_U64,
      .value.n64 = v,
  };
}

[[nodiscard]] static LogEntry log_entry_slice(String k, String v) {
  return (LogEntry){
      .key = k,
      .value.kind = LV_STRING,
      .value.s = v,
  };
}

#define L(k, v)                                                                \
  (_Generic((v),                                                               \
      int: log_entry_int,                                                      \
      u16: log_entry_u16,                                                      \
      u32: log_entry_u32,                                                      \
      u64: log_entry_u64,                                                      \
      String: log_entry_slice)((S(k)), v))

#define LOG_ARGS_COUNT(...)                                                    \
  (sizeof((LogEntry[]){__VA_ARGS__}) / sizeof(LogEntry))
#define log(level, msg, arena, ...)                                            \
  do {                                                                         \
    Arena tmp_arena = *arena;                                                  \
    String log_line = make_log_line(level, S(msg), &tmp_arena,                 \
                                    LOG_ARGS_COUNT(__VA_ARGS__), __VA_ARGS__); \
    write(1, log_line.data, log_line.len);                                     \
  } while (0)

[[nodiscard]] static String json_escape_string(String entry, Arena *arena) {
  DynU8 sb = {0};
  *dyn_push(&sb, arena) = '"';

  for (u64 i = 0; i < entry.len; i++) {
    u8 c = slice_at(entry, i);
    if ('"' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = '"';
    } else if ('\\' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = '\\';
    } else if ('\b' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = 'b';
    } else if ('\f' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = 'f';
    } else if ('\n' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = 'n';
    } else if ('\r' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = 'r';
    } else if ('\t' == c) {
      *dyn_push(&sb, arena) = '\\';
      *dyn_push(&sb, arena) = 't';
    } else {
      *dyn_push(&sb, arena) = c;
    }
  }
  *dyn_push(&sb, arena) = '"';

  return dyn_slice(String, sb);
}

[[nodiscard]] static String json_unescape_string(String entry, Arena *arena) {
  DynU8 sb = {0};

  for (u64 i = 0; i < entry.len; i++) {
    u8 c = slice_at(entry, i);
    u8 next = i + 1 < entry.len ? slice_at(entry, i + 1) : 0;

    if ('\\' == c) {
      if ('"' == next) {
        *dyn_push(&sb, arena) = '"';
        i += 1;
      } else if ('\\' == next) {
        *dyn_push(&sb, arena) = '\\';
        i += 1;
      } else if ('b' == next) {
        *dyn_push(&sb, arena) = '\b';
        i += 1;
      } else if ('f' == next) {
        *dyn_push(&sb, arena) = '\f';
        i += 1;
      } else if ('n' == next) {
        *dyn_push(&sb, arena) = '\n';
        i += 1;
      } else if ('r' == next) {
        *dyn_push(&sb, arena) = '\r';
        i += 1;
      } else if ('t' == next) {
        *dyn_push(&sb, arena) = '\t';
        i += 1;
      } else {
        *dyn_push(&sb, arena) = c;
      }
    } else {
      *dyn_push(&sb, arena) = c;
    }
  }

  return dyn_slice(String, sb);
}

static void dynu8_append_json_object_key_string_value_string(DynU8 *sb,
                                                             String key,
                                                             String value,
                                                             Arena *arena) {
  String json_key = json_escape_string(key, arena);
  dyn_append_slice(sb, json_key, arena);

  dyn_append_slice(sb, S(":"), arena);

  String json_value = json_escape_string(value, arena);
  dyn_append_slice(sb, json_value, arena);

  dyn_append_slice(sb, S(","), arena);
}

static void dynu8_append_json_object_key_string_value_u64(DynU8 *sb, String key,
                                                          u64 value,
                                                          Arena *arena) {
  String json_key = json_escape_string(key, arena);
  dyn_append_slice(sb, json_key, arena);

  dyn_append_slice(sb, S(":"), arena);

  dynu8_append_u64(sb, value, arena);

  dyn_append_slice(sb, S(","), arena);
}

[[nodiscard]] static String make_log_line(LogLevel level, String msg,
                                          Arena *arena, i32 args_count, ...) {
  struct timespec monotonic = {0};
  clock_gettime(CLOCK_MONOTONIC, &monotonic);
  u64 monotonic_ns =
      (u64)monotonic.tv_sec * 1000'000'000 + (u64)monotonic.tv_nsec;

  struct timespec now = {0};
  clock_gettime(CLOCK_REALTIME, &now);
  u64 timestamp_ns = (u64)now.tv_sec * 1000'000'000 + (u64)now.tv_nsec;

  DynU8 sb = {0};
  *dyn_push(&sb, arena) = '{';

  dynu8_append_json_object_key_string_value_string(
      &sb, S("level"), log_level_to_string(level), arena);
  dynu8_append_json_object_key_string_value_u64(&sb, S("timestamp_ns"),
                                                timestamp_ns, arena);
  dynu8_append_json_object_key_string_value_u64(&sb, S("monotonic_ns"),
                                                monotonic_ns, arena);
  dynu8_append_json_object_key_string_value_string(&sb, S("message"), msg,
                                                   arena);

  va_list argp = {0};
  va_start(argp, args_count);
  for (i32 i = 0; i < args_count; i++) {
    LogEntry entry = va_arg(argp, LogEntry);

    switch (entry.value.kind) {
    case LV_STRING: {
      dynu8_append_json_object_key_string_value_string(&sb, entry.key,
                                                       entry.value.s, arena);
      break;
    }
    case LV_U64:
      dynu8_append_json_object_key_string_value_u64(&sb, entry.key,
                                                    entry.value.n64, arena);
      break;
    default:
      ASSERT(0 && "invalid LogValueKind");
    }
  }
  va_end(argp);

  ASSERT(string_ends_with(dyn_slice(String, sb), S(",")));
  dyn_pop(&sb);
  dyn_append_slice(&sb, S("}\n"), arena);

  return dyn_slice(String, sb);
}

[[nodiscard]] static Error os_sendfile(int fd_in, int fd_out, u64 n_bytes) {
#if defined(__linux__)
  ssize_t res = sendfile(fd_out, fd_in, nullptr, n_bytes);
  if (res == -1) {
    return (Error)errno;
  }
  if (res != (ssize_t)n_bytes) {
    return (Error)EAGAIN;
  }
  return 0;
#elif defined(__FreeBSD__)
  int res = sendfile(fd_in, fd_out, 0, n_bytes, nullptr, nullptr, 0);
  if (res == -1) {
    return (Error)errno;
  }
  return 0;
#else
#error "sendfile(2) not implemented on other OSes than Linux/FreeBSD."
#endif
}

typedef struct {
  String *data;
  u64 len;
} StringSlice;

[[nodiscard]] static String json_encode_string_slice(StringSlice strings,
                                                     Arena *arena) {
  DynU8 sb = {0};
  *dyn_push(&sb, arena) = '[';

  for (u64 i = 0; i < strings.len; i++) {
    String s = dyn_at(strings, i);
    String encoded = json_escape_string(s, arena);
    dyn_append_slice(&sb, encoded, arena);

    if (i + 1 < strings.len) {
      *dyn_push(&sb, arena) = ',';
    }
  }

  *dyn_push(&sb, arena) = ']';

  return dyn_slice(String, sb);
}

typedef struct {
  Error err;
  StringSlice string_slice;
} JsonParseStringStrResult;

typedef enum {
  HS_ERR_INVALID_HTTP_REQUEST,
  HS_ERR_INVALID_HTTP_RESPONSE,
  HS_ERR_INVALID_FORM_DATA,
  HS_ERR_INVALID_JSON,
} HS_Error;

[[nodiscard]] static i64 string_indexof_unescaped_byte(String haystack,
                                                       u8 needle) {
  for (u64 i = 0; i < haystack.len; i++) {
    u8 c = slice_at(haystack, i);

    if (c != needle) {
      continue;
    }

    if (i == 0) {
      return (i64)i;
    }

    u8 previous = slice_at(haystack, i - 1);
    if ('\\' != previous) {
      return (i64)i;
    }
  }

  return -1;
}

static u64 skip_over_whitespace(String s, u64 idx_start) {
  ASSERT(idx_start < s.len);

  u64 idx = idx_start;
  for (; idx < s.len; idx++) {
    u8 c = slice_at(s, idx);
    if (' ' != c) {
      return idx;
    }
  }

  return idx;
}

[[nodiscard]] static JsonParseStringStrResult
json_decode_string_slice(String s, Arena *arena) {
  JsonParseStringStrResult res = {0};
  if (s.len < 2) {
    res.err = HS_ERR_INVALID_JSON;
    return res;
  }
  if ('[' != slice_at(s, 0)) {
    res.err = HS_ERR_INVALID_JSON;
    return res;
  }

  DynString dyn = {0};
  for (u64 i = 1; i < s.len - 2;) {
    i = skip_over_whitespace(s, i);

    u8 c = slice_at(s, i);
    if ('"' != c) { // Opening quote.
      res.err = HS_ERR_INVALID_JSON;
      return res;
    }
    i += 1;

    String remaining = slice_range(s, i, 0);
    i64 end_quote_idx = string_indexof_unescaped_byte(remaining, '"');
    if (-1 == end_quote_idx) {
      res.err = HS_ERR_INVALID_JSON;
      return res;
    }

    ASSERT(0 <= end_quote_idx);

    String str = slice_range(s, i, i + (u64)end_quote_idx);
    String unescaped = json_unescape_string(str, arena);
    *dyn_push(&dyn, arena) = unescaped;

    i += (u64)end_quote_idx;

    if ('"' != c) { // Closing quote.
      res.err = HS_ERR_INVALID_JSON;
      return res;
    }
    i += 1;

    i = skip_over_whitespace(s, i);
    if (i + 1 == s.len) {
      break;
    }

    c = slice_at(s, i);
    if (',' != c) {
      res.err = HS_ERR_INVALID_JSON;
      return res;
    }
    i += 1;
  }

  if (']' != slice_at(s, s.len - 1)) {
    res.err = HS_ERR_INVALID_JSON;
    return res;
  }

  res.string_slice = dyn_slice(StringSlice, dyn);
  return res;
}

[[nodiscard]] static String string_clone(String s, Arena *arena) {
  String res = {.data = arena_alloc(arena, s.len, 1, 1), .len = s.len};
  if (res.data != nullptr) {
    memcpy(res.data, s.data, s.len);
  }

  return res;
}

static void string_lowercase_ascii_mut(String s) {
  for (u64 i = 0; i < s.len; i++) {
    u8 *c = AT_PTR(s.data, s.len, i);
    if ('A' <= *c && *c <= 'Z') {
      *c += 32;
    }
  }
}

[[nodiscard]] static bool string_ieq_ascii(String a, String b, Arena *arena) {
  if (a.data == nullptr && b.data == nullptr && a.len == b.len) {
    return true;
  }
  if (a.data == nullptr) {
    return false;
  }
  if (b.data == nullptr) {
    return false;
  }

  if (a.len != b.len) {
    return false;
  }

  ASSERT(a.data != nullptr);
  ASSERT(b.data != nullptr);
  ASSERT(a.len == b.len);

  Arena tmp = *arena;
  String a_clone = string_clone(a, &tmp);
  String b_clone = string_clone(b, &tmp);

  string_lowercase_ascii_mut(a_clone);
  string_lowercase_ascii_mut(b_clone);

  return string_eq(a_clone, b_clone);
}
