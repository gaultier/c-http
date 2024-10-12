#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint64_t MAX_REQUEST_LINES = 512;

#define ASSERT(x)                                                              \
  do {                                                                         \
    if (!(x)) {                                                                \
      abort();                                                                 \
    }                                                                          \
  } while (0)

typedef enum {
  HS_ERR_INVALID_HTTP_REQUEST,
} HS_ERROR;

typedef struct {
  uint8_t *data;
  uint64_t len;
} Slice;

static Slice slice_make_from_cstr(char *s) {
  const uint64_t s_len = strlen(s);
  const Slice slice = {.data = (uint8_t *)s, .len = s_len};
  return slice;
}

static Slice slice_trim_left(Slice s, uint8_t c) {
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

static Slice slice_trim_right(Slice s, uint8_t c) {
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

static Slice slice_trim(Slice s, uint8_t c) {
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

static SplitIterator slice_split_it(Slice slice, uint8_t sep) {
  return (SplitIterator){.slice = slice, .sep = sep};
}

static int64_t slice_indexof_byte(Slice haystack, uint8_t needle) {
  if (haystack.data == NULL) {
    return -1;
  }

  if (haystack.len == 0) {
    return -1;
  }

  const uint8_t *res = memchr(haystack.data, needle, haystack.len);
  if (res == NULL) {
    return -1;
  }

  return res - haystack.data;
}

static Slice slice_range(Slice src, uint64_t start, uint64_t end) {
  const uint64_t real_end = end == 0 ? src.len : end;
  ASSERT(start <= real_end);
  ASSERT(start <= src.len);
  ASSERT(real_end <= src.len);

  Slice res = {.data = src.data + start, .len = real_end - start};
  return res;
}

static SplitResult slice_split_next(SplitIterator *it) {
  const int64_t idx = slice_indexof_byte(it->slice, it->sep);
  if (-1 == idx) {
    return (SplitResult){};
  }
  SplitResult res = {.slice = slice_range(it->slice, 0, idx), .ok = true};
  it->slice = slice_range(it->slice, idx + 1, 0);

  return res;
}

static const Slice NEWLINE = {.data = (uint8_t *)"\r\n", .len = 2};

typedef enum { HM_UNKNOWN, HM_GET, HM_POST } HttpMethod;

typedef struct {
  Slice path;
  HttpMethod method;
  int err;
} HttpRequestRead;

typedef struct {
  uint16_t status;
} HttpResponse;

typedef struct {
  uint8_t *start;
  uint8_t *end;
} Arena;

typedef struct {
  uint8_t *data;
  uint64_t len, cap;
} DynArrayU8;

static void *arena_alloc(Arena *a, uint64_t size, uint64_t align,
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

  replica.cap = replica.cap ? replica.cap : 1;
  void *data = arena_alloc(a, 2 * size, align, replica.cap);
  replica.cap *= 2;
  if (replica.len) {
    memcpy(data, replica.data, size * replica.len);
  }
  replica.data = data;

  ASSERT(NULL != slice);

  memcpy(slice, &replica, sizeof(replica));
}

#define dyn_push(s, arena)                                                     \
  ((s)->len >= (s)->cap                                                        \
   ? dyn_grow(s, sizeof(*(s)->data), _Alignof(*(s)->data), arena),             \
   (s)->data + (s)->len++ : (s)->data + (s)->len++)

#define dyn_append_slice(dst, src, arena)                                      \
  do {                                                                         \
    for (uint64_t i = 0; i < src.len; i++) {                                   \
      *dyn_push(dst, arena) = src.data[i];                                     \
    }                                                                          \
  } while (0)

static Slice dyn_array_u8_to_slice(DynArrayU8 dyn) {
  return (Slice){.data = dyn.data, .len = dyn.len};
}

static void dyn_array_u8_append_cstr(DynArrayU8 *dyn, char *s, Arena *arena) {
  dyn_append_slice(dyn, slice_make_from_cstr(s), arena);
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

static Arena arena_make(uint64_t size) {
  void *ptr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

  if (ptr == NULL) {
    fprintf(stderr, "failed to mmap: %d %s\n", errno, strerror(errno));
    exit(errno);
  }
  return (Arena){.start = ptr, .end = ptr + size};
}

typedef struct {
  uint64_t bytes_count;
  int err;
} IoOperationResult;

typedef IoOperationResult (*ReadFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  uint64_t buf_idx;
  DynArrayU8 buf;
  void *ctx;
  ReadFn read_fn;
} LineBufferedReader;

static IoOperationResult
line_buffered_reader_read_from_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_read = recv((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_read == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.bytes_count = n_read};
}

static LineBufferedReader line_buffered_reader_make_from_socket(int socket) {
  return (LineBufferedReader){
      .ctx = (void *)(uint64_t)socket,
      .read_fn = line_buffered_reader_read_from_socket,
  };
}

typedef IoOperationResult (*WriteFn)(void *ctx, const void *buf,
                                     size_t buf_len);

typedef struct {
  WriteFn write;
  void *ctx;
} Writer;

static IoOperationResult writer_write_from_socket(void *ctx, const void *buf,
                                                  size_t buf_len) {
  const ssize_t n_written = send((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_written == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.bytes_count = n_written};
}

static Writer writer_make_from_socket(int socket) {
  return (Writer){
      .ctx = (void *)(uint64_t)socket,
      .write = writer_write_from_socket,
  };
}

static int writer_write_all(Writer writer, Slice slice) {
  for (uint64_t idx = 0; idx < slice.len;) {
    const Slice to_write = slice_range(slice, idx, 0);
    const IoOperationResult write_res =
        writer.write(writer.ctx, to_write.data, to_write.len);
    if (write_res.err) {
      return write_res.err;
    }

    ASSERT(write_res.bytes_count <= slice.len);
    ASSERT(idx <= slice.len);
    idx += write_res.bytes_count;
    ASSERT(idx <= slice.len);
  }
  return 0;
}

typedef struct {
  Slice line;
  int err;
  bool present;
} LineRead;

static bool slice_eq(Slice a, Slice b) {
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

static int64_t slice_indexof_slice(Slice haystack, Slice needle) {
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

static Slice dyn_array_u8_range(DynArrayU8 src, uint64_t start, uint64_t end) {
  Slice src_slice = {.data = src.data, .len = src.len};
  return slice_range(src_slice, start, end);
}

static LineRead line_buffered_reader_consume(LineBufferedReader *reader) {
  LineRead res = {0};

  if (reader->buf_idx >= reader->buf.len) {
    return res;
  }

  const Slice to_search = dyn_array_u8_range(reader->buf, reader->buf_idx, 0);
  const int64_t newline_idx = slice_indexof_slice(to_search, NEWLINE);

  if (newline_idx == -1) {
    return res;
  }

  res.line = dyn_array_u8_range(reader->buf, reader->buf_idx,
                                reader->buf_idx + newline_idx);
  res.present = true;
  reader->buf_idx += newline_idx + NEWLINE.len;

  return res;
}

static LineRead line_buffered_reader_read(LineBufferedReader *reader,
                                          Arena *arena) {
  LineRead line = {0};

  for (uint64_t _i = 0; _i < 10; _i++) {
    line = line_buffered_reader_consume(reader);
    if (line.present) {
      return line;
    }

    uint8_t buf[4096] = {0};
    const IoOperationResult io_result =
        reader->read_fn(reader->ctx, buf, sizeof(buf));
    if (io_result.err) {
      continue; // Retry. Should we just abort?
    }

    const Slice slice = {.data = buf, .len = io_result.bytes_count};
    dyn_append_slice(&reader->buf, slice, arena);
  }
  return line;
}

HttpRequestRead request_parse_status_line(Slice s) {}

/* fn request_parse_status_line(s: []const u8) !HttpRequest { */
/*     std.log.info("status line `{s}`", .{s}); */
/*     const space = [_]u8{ ' ', '\r' }; */

/*     var it = std.mem.splitScalar(u8, s, ' '); */

/*     var req: HttpRequest = undefined; */
/*     if (it.next()) |method| { */
/*         const method_trimmed = std.mem.trim(u8, method, space[0..]); */

/*         if (std.mem.eql(u8, method_trimmed, "GET")) { */
/*             req.method = .Get; */
/*         } else if (std.mem.eql(u8, method_trimmed, "POST")) { */
/*             req.method = .Post; */
/*         } else { */
/*             std.log.err("invalid http method `{s}` `{s}`", .{ s, method });
 */
/*             return error.InvalidHttpMethod; */
/*         } */
/*     } else { */
/*         return error.MissingHttpMethod; */
/*     } */

/*     if (it.next()) |path| { */
/*         const path_trimmed = std.mem.trim(u8, path, space[0..]); */
/*         req.path = path_trimmed; */
/*     } else { */
/*         return error.InvalidUri; */
/*     } */

/*     if (it.next()) |http_version| { */
/*         const http_version_trimmed = std.mem.trim(u8, http_version,
 * space[0..]); */
/*         std.log.info("http_version=`{s}`", .{http_version_trimmed}); */
/*         if (!std.mem.eql(u8, http_version_trimmed, "HTTP/1.1")) { */
/*             return error.InvalidHttpVersion; */
/*         } */
/*     } */

/*     return req; */
/* } */

static HttpRequestRead request_read(LineBufferedReader *reader, Arena *arena) {
  HttpRequestRead res = {0};

  const LineRead status_line = line_buffered_reader_read(reader, arena);
  if (!status_line.present) {
    res.err = HS_ERR_INVALID_HTTP_REQUEST;
    return res;
  }
  if (status_line.err) {
    res.err = status_line.err;
    return res;
  }

  for (uint64_t _i = 0; _i < MAX_REQUEST_LINES; _i++) {
    const LineRead line = line_buffered_reader_read(reader, arena);

    if (line.err) {
      res.err = line.err;
      return res;
    }

    if (!line.present || line.line.len == 0) {
      break;
    }
  }

  return res;
}

static int response_write(Writer writer, HttpResponse res, Arena *arena) {
  DynArrayU8 sb = {0};

  dyn_array_u8_append_cstr(&sb, "HTTP/1.1 200\r\n", arena);
  dyn_array_u8_append_cstr(&sb, "\r\n", arena);

  const Slice slice = dyn_array_u8_to_slice(sb);
  return writer_write_all(writer, slice);
}

static void handle_client(int socket) {
  Arena arena = arena_make(4096);
  LineBufferedReader reader = line_buffered_reader_make_from_socket(socket);
  const HttpRequestRead req = request_read(&reader, &arena);
  if (req.err) {
    exit(EINVAL);
  }

  const HttpResponse res = {.status = 201};
  Writer writer = writer_make_from_socket(socket);
  response_write(writer, res, &arena);
}

static int run() {
  const uint16_t port = 12345;
  struct sigaction sa = {.sa_flags = SA_NOCLDWAIT};
  int err = 0;
  if (-1 == (err = sigaction(SIGCHLD, &sa, NULL))) {
    fprintf(stderr, "Failed to sigaction(2): err=%s\n", strerror(errno));
    exit(errno);
  }

  const int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd == -1) {
    fprintf(stderr, "Failed to socket(2): %s\n", strerror(errno));
    exit(errno);
  }

  int val = 1;
  if ((err = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &val,
                        sizeof(val))) == -1) {
    fprintf(stderr, "Failed to setsockopt(2): %s\n", strerror(errno));
    exit(errno);
  }
#ifdef __FreeBSD__
  if ((err = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val,
                        sizeof(val))) == -1) {
    fprintf(stderr, "Failed to setsockopt(2): %s\n", strerror(errno));
    exit(errno);
  }
#endif

  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };

  if ((err = bind(sock_fd, (const struct sockaddr *)&addr, sizeof(addr))) ==
      -1) {
    fprintf(stderr, "Failed to bind(2): %s\n", strerror(errno));
    exit(errno);
  }

  if ((err = listen(sock_fd, 16 * 1024)) == -1) {
    fprintf(stderr, "Failed to listen(2): %s\n", strerror(errno));
    exit(errno);
  }

  while (1) {
    const int conn_fd = accept(sock_fd, NULL, 0);
    if (conn_fd == -1) {
      fprintf(stderr, "Failed to accept(2): %s\n", strerror(errno));
      return errno;
    }

    const pid_t pid = fork();
    if (pid == -1) {
      fprintf(stderr, "Failed to fork(2): err=%s\n", strerror(errno));
      exit(errno);
    } else if (pid == 0) { // Child
      handle_client(conn_fd);
      exit(0);
    } else { // Parent
      // Fds are duplicated by fork(2) and need to be
      // closed by both parent & child
      close(conn_fd);
    }
  }
}
