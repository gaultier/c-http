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
  (((int64_t)(idx) >= (int64_t)(len))                                          \
       ? (__builtin_trap(), &(arr)[0])                                         \
       : (ASSERT(nullptr != arr), (&(arr)[idx])))

#define AT(arr, len, idx) (*AT_PTR(arr, len, idx))

typedef uint32_t Error;

[[nodiscard]] static bool ch_is_hex_digit(uint8_t c) {
  return ('0' <= c && c <= '9') || ('A' <= c && c <= 'F') ||
         ('a' <= c && c <= 'f');
}

[[nodiscard]] static uint8_t ch_from_hex(uint8_t c) {
  ASSERT(ch_is_hex_digit(c));

  if ('0' <= c && c <= '9') {
    return c - '0';
  }

  if ('A' <= c && c <= 'F') {
    return c - 'A';
  }

  if ('a' <= c && c <= 'f') {
    return c - 'a';
  }

  ASSERT(false);
}

typedef struct {
  uint8_t *data;
  uint64_t len;
} Slice;

[[nodiscard]] static bool slice_is_empty(Slice s) {
  if (s.len == 0) {
    return true;
  }

  ASSERT(s.data != nullptr);
  return false;
}

#define S(s) ((Slice){.data = (uint8_t *)s, .len = sizeof(s) - 1})

[[nodiscard]] static Slice slice_trim_left(Slice s, uint8_t c) {
  Slice res = s;

  for (uint64_t s_i = 0; s_i < s.len; s_i++) {
    ASSERT(s.data != nullptr);
    if (AT(s.data, s.len, s_i) != c) {
      return res;
    }

    res.data += 1;
    res.len -= 1;
  }
  return res;
}

[[nodiscard]] static Slice slice_trim_right(Slice s, uint8_t c) {
  Slice res = s;

  for (int64_t s_i = (int64_t)s.len - 1; s_i >= 0; s_i--) {
    ASSERT(s.data != nullptr);
    if (AT(s.data, s.len, s_i) != c) {
      return res;
    }

    res.len -= 1;
  }
  return res;
}

[[nodiscard]] static Slice slice_trim(Slice s, uint8_t c) {
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

[[nodiscard]] static SplitIterator slice_split(Slice slice, uint8_t sep) {
  return (SplitIterator){.slice = slice, .sep = sep};
}

[[nodiscard]] static int64_t slice_indexof_byte(Slice haystack,
                                                uint8_t needle) {
  if (slice_is_empty(haystack)) {
    return -1;
  }

  const uint8_t *res = memchr(haystack.data, needle, haystack.len);
  if (res == nullptr) {
    return -1;
  }

  return res - haystack.data;
}

[[nodiscard]] static Slice slice_range(Slice src, uint64_t start,
                                       uint64_t end) {
  const uint64_t real_end = end == 0 ? src.len : end;
  ASSERT(start <= real_end);
  ASSERT(start <= src.len);
  ASSERT(real_end <= src.len);

  Slice res = {.data = src.data + start, .len = real_end - start};
  return res;
}

[[nodiscard]] static SplitResult slice_split_next(SplitIterator *it) {
  if (slice_is_empty(it->slice)) {
    return (SplitResult){0};
  }

  for (uint64_t _i = 0; _i < it->slice.len; _i++) {
    const int64_t idx = slice_indexof_byte(it->slice, it->sep);
    if (-1 == idx) {
      // Last element.
      SplitResult res = {.slice = it->slice, .ok = true};
      it->slice = (Slice){0};
      return res;
    }

    if (idx == 0) { // Multiple contiguous separators.
      it->slice = slice_range(it->slice, (uint64_t)idx + 1, 0);
      continue;
    } else {
      SplitResult res = {.slice = slice_range(it->slice, 0, (uint64_t)idx),
                         .ok = true};
      it->slice = slice_range(it->slice, (uint64_t)idx + 1, 0);

      return res;
    }
  }
  return (SplitResult){0};
}

[[nodiscard]] static bool slice_eq(Slice a, Slice b) {
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

[[nodiscard]] static int64_t slice_indexof_slice(Slice haystack, Slice needle) {
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

  uint64_t res = (uint64_t)((uint8_t *)ptr - haystack.data);
  ASSERT(res < haystack.len);
  return (int64_t)res;
}

[[nodiscard]] static bool slice_starts_with(Slice haystack, Slice needle) {
  int64_t idx = slice_indexof_slice(haystack, needle);
  return idx == 0;
}

[[maybe_unused]] [[nodiscard]] static bool slice_ends_with(Slice haystack,
                                                           Slice needle) {
  int64_t idx = slice_indexof_slice(haystack, needle);
  return idx == (int64_t)haystack.len - (int64_t)needle.len;
}

typedef struct {
  uint64_t n;
  bool err;
  bool present;
} ParseNumberResult;

[[nodiscard]] static ParseNumberResult slice_parse_u64_decimal(Slice slice) {
  Slice trimmed = slice_trim(slice, ' ');

  ParseNumberResult res = {0};

  for (uint64_t i = 0; i < trimmed.len; i++) {
    uint8_t c = AT(trimmed.data, trimmed.len, i);

    if (!('0' <= c && c <= '9')) { // Error.
      res.err = true;
      return res;
    }

    res.n *= 10;
    res.n += (uint8_t)AT(trimmed.data, trimmed.len, i) - '0';
  }
  res.present = true;
  return res;
}

typedef struct {
  uint8_t *start;
  uint8_t *end;
} Arena;

__attribute((malloc, alloc_size(2, 4), alloc_align(3)))
[[nodiscard]] static void *
arena_alloc(Arena *a, uint64_t size, uint64_t align, uint64_t count) {
  ASSERT(a->start != nullptr);

  const uint64_t padding = (-(uint64_t)a->start & (align - 1));
  ASSERT(padding <= align);

  const int64_t available =
      (int64_t)a->end - (int64_t)a->start - (int64_t)padding;
  ASSERT(available >= 0);
  ASSERT(count <= (uint64_t)available / size);

  void *res = a->start + padding;
  ASSERT(res != nullptr);
  ASSERT(res <= (void *)a->end);

  a->start += padding + count * size;
  ASSERT(a->start <= a->end);
  ASSERT((uint64_t)a->start % align == 0); // Aligned.

  return memset(res, 0, count * size);
}

[[nodiscard]] static char *slice_to_cstr(Slice s, Arena *arena) {
  char *res = arena_alloc(arena, 1, 1, s.len + 1);
  if (NULL != s.data) {
    memcpy(res, s.data, s.len);
  }
  return res;
}

static void dyn_grow(void *slice, uint64_t size, uint64_t align, uint64_t count,
                     Arena *a) {
  ASSERT(nullptr != slice);

  struct {
    void *data;
    uint64_t len;
    uint64_t cap;
  } replica;

  memcpy(&replica, slice, sizeof(replica));
  ASSERT(replica.cap < count);

  uint64_t new_cap = replica.cap == 0 ? 2 : replica.cap;
  for (uint64_t i = 0; i < 64; i++) {
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

  uint64_t array_end = 0;
  uint64_t array_bytes_count = 0;
  ASSERT(false == ckd_mul(&array_bytes_count, size, replica.cap));
  ASSERT(false ==
         ckd_add(&array_end, (uint64_t)replica.data, array_bytes_count));
  ASSERT((uint64_t)replica.data <= array_end);
  ASSERT(array_end < (uint64_t)a->end);

  if (nullptr ==
      replica.data) { // First allocation ever for this dynamic array.
    replica.data = arena_alloc(a, size, align, new_cap);
  } else if ((uint64_t)a->start == array_end) { // Optimization.
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
  uint8_t *data;
  uint64_t len, cap;
} DynArrayU8;

typedef struct {
  Slice *data;
  uint64_t len, cap;
} DynArraySlice;

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
    for (uint64_t _iii = 0; _iii < src.len; _iii++) {                          \
      *dyn_push(dst, arena) = AT(src.data, src.len, _iii);                     \
    }                                                                          \
  } while (0)

[[nodiscard]] static Slice dyn_array_u8_to_slice(DynArrayU8 dyn) {
  return (Slice){.data = dyn.data, .len = dyn.len};
}

static void dyn_array_u8_append_u64(DynArrayU8 *dyn, uint64_t n, Arena *arena) {
  uint8_t tmp[30] = {0};
  const int written_count = snprintf((char *)tmp, sizeof(tmp), "%lu", n);

  ASSERT(written_count > 0);

  Slice slice = {.data = tmp, .len = (uint64_t)written_count};
  dyn_append_slice(dyn, slice, arena);
}

// FIXME: Proper encoding & rm.
[[maybe_unused]] static void
dyn_append_length_prefixed_slice(DynArrayU8 *dyn, Slice slice, Arena *arena) {
  Slice len = {
      .data = (uint8_t *)&slice.len,
      .len = sizeof(slice.len),
  };
  dyn_append_slice(dyn, len, arena);
  dyn_append_slice(dyn, slice, arena);
}

[[nodiscard]] static uint8_t u8_to_ch_hex(uint8_t n) {
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

static void dyn_array_u8_append_u128_hex(DynArrayU8 *dyn, __uint128_t n,
                                         Arena *arena) {
  dyn_ensure_cap(dyn, dyn->len + 32, arena);
  uint64_t dyn_original_len = dyn->len;

  uint8_t it[16] = {0};
  ASSERT(sizeof(it) == sizeof(n));
  memcpy(it, (uint8_t *)&n, sizeof(n));

  for (uint64_t i = 0; i < sizeof(it); i++) {
    uint8_t c1 = it[i] % 16;
    uint8_t c2 = it[i] / 16;
    *dyn_push(dyn, arena) = u8_to_ch_hex(c2);
    *dyn_push(dyn, arena) = u8_to_ch_hex(c1);
  }
  ASSERT(32 == (dyn->len - dyn_original_len));
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

[[nodiscard]] static uint64_t round_up_multiple_of(uint64_t n,
                                                   uint64_t multiple) {
  ASSERT(0 != multiple);

  if (0 == n % multiple) {
    return n; // No-op.
  }

  uint64_t factor = n / multiple;
  return (factor + 1) * n;
}

[[nodiscard]] static Arena arena_make_from_virtual_mem(uint64_t size) {
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGE_SIZE); // FIXME
  uint64_t alloc_real_size = round_up_multiple_of(size, page_size);
  ASSERT(0 == alloc_real_size % page_size);

  uint64_t mmap_size = alloc_real_size;
  // Page guard before.
  ASSERT(false == ckd_add(&mmap_size, mmap_size, page_size));
  // Page guard after.
  ASSERT(false == ckd_add(&mmap_size, mmap_size, page_size));

  uint8_t *alloc = mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
  ASSERT(nullptr != alloc);

  uint64_t page_guard_before = (uint64_t)alloc;

  ASSERT(false == ckd_add((uint64_t *)&alloc, (uint64_t)alloc, page_size));
  ASSERT(page_guard_before + page_size == (uint64_t)alloc);

  uint64_t page_guard_after = (uint64_t)0;
  ASSERT(false == ckd_add(&page_guard_after, (uint64_t)alloc, alloc_real_size));
  ASSERT((uint64_t)alloc + alloc_real_size == page_guard_after);
  ASSERT(page_guard_before + page_size + alloc_real_size == page_guard_after);

  ASSERT(0 == mprotect((void *)page_guard_before, page_size, PROT_NONE));
  ASSERT(0 == mprotect((void *)page_guard_after, page_size, PROT_NONE));

  // Trigger a page fault preemptively to detect invalid virtual memory
  // mappings.
  *(uint8_t *)alloc = 0;

  return (Arena){.start = alloc, .end = (uint8_t *)alloc + size};
}

typedef enum {
  LV_SLICE,
  LV_U64,
  LV_U128,
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
    Slice s;
    uint64_t n64;
    __uint128_t n128;
  };
} LogValue;

typedef struct {
  Slice key;
  LogValue value;
} LogEntry;

#define LCI(k, v)                                                              \
  ((LogEntry){                                                                 \
      .key = S(k),                                                             \
      .value.kind = LV_U64,                                                    \
      .value.n64 = v,                                                          \
  })

#define LCII(k, v)                                                             \
  ((LogEntry){                                                                 \
      .key = S(k),                                                             \
      .value.kind = LV_U128,                                                   \
      .value.n128 = v,                                                         \
  })

#define LCS(k, v)                                                              \
  ((LogEntry){                                                                 \
      .key = S(k),                                                             \
      .value.kind = LV_SLICE,                                                  \
      .value.s = v,                                                            \
  })

#define LOG_ARGS_COUNT(...)                                                    \
  (sizeof((LogEntry[]){__VA_ARGS__}) / sizeof(LogEntry))
#define log(level, msg, arena, ...)                                            \
  do {                                                                         \
    Arena tmp_arena = *arena;                                                  \
    Slice log_line = make_log_line(level, S(msg), &tmp_arena,                  \
                                   LOG_ARGS_COUNT(__VA_ARGS__), __VA_ARGS__);  \
    write(1, log_line.data, log_line.len);                                     \
  } while (0)

[[nodiscard]] static Slice log_entry_quote_value(Slice entry, Arena *arena) {
  DynArrayU8 sb = {0};
  *dyn_push(&sb, arena) = '"';

  for (uint64_t i = 0; i < entry.len; i++) {
    uint8_t c = AT(entry.data, entry.len, i);
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

[[nodiscard]] static Slice make_log_line(LogLevel level, Slice msg,
                                         Arena *arena, int32_t args_count,
                                         ...) {
  struct timespec now = {0};
  clock_gettime(CLOCK_MONOTONIC, &now);

  DynArrayU8 sb = {0};

  dyn_append_slice(&sb, S("level="), arena);
  switch (level) {
  case LOG_LEVEL_DEBUG:
    dyn_append_slice(&sb, S("debug"), arena);
    break;
  case LOG_LEVEL_INFO:
    dyn_append_slice(&sb, S("info"), arena);
    break;
  case LOG_LEVEL_ERROR:
    dyn_append_slice(&sb, S("error"), arena);
    break;
  case LOG_LEVEL_FATAL:
    dyn_append_slice(&sb, S("fatal"), arena);
    break;
  default:
    ASSERT(false);
  }
  dyn_append_slice(&sb, S(" "), arena);

  dyn_append_slice(&sb, S("timestamp_ns="), arena);
  dyn_array_u8_append_u64(
      &sb, (uint64_t)now.tv_sec * 1000'000'000 + (uint64_t)now.tv_nsec, arena);
  dyn_append_slice(&sb, S(" "), arena);

  dyn_append_slice(&sb, S("message="), arena);
  dyn_append_slice(&sb, log_entry_quote_value(msg, arena), arena);
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
      dyn_append_slice(&sb, S("\""), arena);
      dyn_array_u8_append_u128_hex(&sb, entry.value.n128, arena);
      dyn_append_slice(&sb, S("\""), arena);
      break;
    default:
      ASSERT(0 && "invalid LogValueKind");
    }

    dyn_append_slice(&sb, S(" "), arena);
  }
  va_end(argp);

  ASSERT(' ' == *dyn_last_ptr(&sb));
  dyn_pop(&sb);
  dyn_append_slice(&sb, S("\n"), arena);

  return dyn_array_u8_to_slice(sb);
}

static Error os_sendfile(int fd_in, int fd_out, uint64_t n_bytes) {
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
