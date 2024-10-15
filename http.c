#pragma once

#include "lib.c"

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const uint64_t MAX_REQUEST_LINES = 512;
static const uint64_t CLIENT_MEM = 8192;
static const uint16_t PORT = 12345;
static const int LISTEN_BACKLOG = 1024;

typedef enum {
  HS_ERR_INVALID_HTTP_REQUEST,
} HS_ERROR;

typedef enum { HM_UNKNOWN, HM_GET, HM_POST } HttpMethod;

Slice static http_method_to_s(HttpMethod m) {
  switch (m) {
  case HM_UNKNOWN:
    return S("unknown");
  case HM_GET:
    return S("GET");
  case HM_POST:
    return S("POST");
  default:
    ASSERT(0);
  }
}

typedef struct {
  Slice key, value;
} HttpHeader;

typedef struct {
  HttpHeader *data;
  uint64_t len, cap;
} DynArrayHttpHeaders;

typedef struct {
  __uint128_t id;
  Slice path; // FIXME: Should be a parsed URL/URI.
  HttpMethod method;
  DynArrayHttpHeaders headers;
  // TODO: fill.
  Slice body;
  int err;
} HttpRequest;

typedef struct {
  uint16_t status;
  DynArrayHttpHeaders headers;
  int err;
  char *file_path;
  Slice body;
} HttpResponse;

typedef struct {
  Slice slice;
  int err;
} IoOperationResult;

typedef IoOperationResult (*ReadFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  uint64_t buf_idx;
  DynArrayU8 buf;
  void *ctx;
  ReadFn read_fn;
} Reader;

#define MAX_READER_READER 4096

MUST_USE static IoOperationResult reader_read_from_socket(void *ctx, void *buf,
                                                          size_t buf_len) {
  const ssize_t n_read = recv((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_read == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.slice.data = buf, .slice.len = n_read};
}

MUST_USE static Reader reader_make_from_socket(int socket) {
  return (Reader){
      .ctx = (void *)(uint64_t)socket,
      .read_fn = reader_read_from_socket,
  };
}

typedef IoOperationResult (*WriteFn)(void *ctx, const void *buf,
                                     size_t buf_len);

typedef struct {
  WriteFn write;
  void *ctx;
} Writer;

MUST_USE static IoOperationResult
writer_write_from_socket(void *ctx, const void *buf, size_t buf_len) {
  const ssize_t n_written = send((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_written == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.slice.data = (uint8_t *)buf,
                             .slice.len = n_written};
}

MUST_USE static Writer writer_make_from_socket(int socket) {
  return (Writer){
      .ctx = (void *)(uint64_t)socket,
      .write = writer_write_from_socket,
  };
}

MUST_USE static int writer_write_all(Writer writer, Slice slice) {
  for (uint64_t idx = 0; idx < slice.len;) {
    const Slice to_write = slice_range(slice, idx, 0);
    const IoOperationResult write_res =
        writer.write(writer.ctx, to_write.data, to_write.len);
    if (write_res.err) {
      return write_res.err;
    }

    ASSERT(write_res.slice.len <= slice.len);
    ASSERT(idx <= slice.len);
    idx += write_res.slice.len;
    ASSERT(idx <= slice.len);
  }
  return 0;
}

typedef struct {
  Slice line;
  int err;
  bool present;
} LineRead;

MUST_USE static IoOperationResult reader_read_from_buffer(Reader *reader) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  if (reader->buf_idx == reader->buf.len) {
    return (IoOperationResult){0};
  }

  IoOperationResult res = {
      .slice.data = &reader->buf.data[reader->buf_idx],
      .slice.len = reader->buf.len - reader->buf_idx,
  };
  reader->buf_idx = reader->buf.len;

  return res;
}

MUST_USE static IoOperationResult _reader_read_from_io(Reader *reader,
                                                       Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  uint8_t tmp[MAX_READER_READER] = {0};
  IoOperationResult res = reader->read_fn(reader->ctx, tmp, sizeof(tmp));
  if (res.err) {
    return res;
  }

  Slice slice = {.data = tmp, .len = res.slice.len};

  uint64_t reader_buf_len_prev = reader->buf.len;
  dyn_append_slice(&reader->buf, slice, arena);

  res.slice.data = &reader->buf.data[reader_buf_len_prev];
  return res;
}

MUST_USE static IoOperationResult reader_read(Reader *reader, Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  IoOperationResult res = reader_read_from_buffer(reader);
  if (!slice_is_empty(res.slice)) {
    return res;
  }

  res = _reader_read_from_io(reader, arena);
  if (res.err) {
    return res;
  }

  return reader_read_from_buffer(reader);
}

MUST_USE static IoOperationResult
reader_read_until_slice(Reader *reader, Slice needle, Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  IoOperationResult io = {0};

  for (uint64_t i = 0; i < 128; i++) // FIXME
  {
    io = reader_read(reader, arena);
    if (io.err) {
      return io;
    }

    int64_t idx = slice_indexof_slice(io.slice, needle);
    // Not found, continue reading.
    if (idx == -1) {
      continue;
    }

    // Found but maybe read some in excess, need to rewind a bit.
    uint64_t excess_read = io.slice.len - (idx + needle.len);
    ASSERT(reader->buf_idx >= excess_read);
    reader->buf_idx -= excess_read;
    io.slice.len = idx;
    return io;
  }

  io.err = EINTR;
  return io;
}

MUST_USE static IoOperationResult
reader_read_exactly(Reader *reader, uint64_t content_length, Arena *arena) {
  uint64_t remaining_to_read = content_length;

  IoOperationResult res = {0};

  for (uint64_t i = 0; i < content_length; i++) {
    if (0 == remaining_to_read) {
      ASSERT(res.slice.len == content_length);
      return res;
    }

    ASSERT(remaining_to_read > 0);

    res = reader_read(reader, arena);
    if (res.err) {
      return res;
    }

    ASSERT(res.slice.len <= remaining_to_read);
    remaining_to_read -= res.slice.len;
  }
  return res;
}

MUST_USE static LineRead reader_read_line(Reader *reader, Arena *arena) {
  const Slice NEWLINE = S("\r\n");

  LineRead line = {0};

  for (uint64_t _i = 0; _i < 10; _i++) {
    const IoOperationResult io_result =
        reader_read_until_slice(reader, NEWLINE, arena);
    if (io_result.err) {
      line.err = io_result.err;
      return line;
    }

    line.present = true;
    line.line = io_result.slice;
    return line;
  }

  return line;
}

MUST_USE static HttpRequest request_parse_status_line(LineRead status_line) {
  HttpRequest req = {0};
  arc4random_buf(&req.id, sizeof(req.id));

  if (!status_line.present) {
    req.err = HS_ERR_INVALID_HTTP_REQUEST;
    return req;
  }
  if (status_line.err) {
    req.err = status_line.err;
    return req;
  }

  SplitIterator it = slice_split_it(status_line.line, ' ');

  {
    SplitResult method = slice_split_next(&it);
    if (!method.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (slice_eq(method.slice, S("GET"))) {
      req.method = HM_GET;
    } else if (slice_eq(method.slice, S("POST"))) {
      req.method = HM_POST;
    } else {
      // FIXME: More.
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }
  }

  {
    SplitResult path = slice_split_next(&it);
    if (!path.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (slice_is_empty(path.slice)) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (path.slice.data[0] != '/') {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    req.path = path.slice;
  }

  {
    SplitResult http_version = slice_split_next(&it);
    if (!http_version.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (!slice_eq(http_version.slice, S("HTTP/1.1"))) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }
  }

  return req;
}

MUST_USE static HttpRequest request_read_headers(HttpRequest req,
                                                 Reader *reader, Arena *arena) {
  ASSERT(!req.err);

  HttpRequest res = req;

  for (uint64_t _i = 0; _i < MAX_REQUEST_LINES; _i++) {
    const LineRead line = reader_read_line(reader, arena);

    if (line.err) {
      res.err = line.err;
      return res;
    }

    if (!line.present || slice_is_empty(line.line)) {
      break;
    }

    SplitIterator it = slice_split_it(line.line, ':');
    SplitResult key = slice_split_next(&it);
    if (!key.ok) {
      res.err = HS_ERR_INVALID_HTTP_REQUEST;
      return res;
    }

    Slice key_trimmed = slice_trim(key.slice, ' ');

    Slice value = it.slice; // Remainder.
    Slice value_trimmed = slice_trim(value, ' ');

    HttpHeader header = {.key = key_trimmed, .value = value_trimmed};
    *dyn_push(&res.headers, arena) = header;
  }
  return res;
}

MUST_USE static ParseNumberResult
request_parse_content_length_maybe(HttpRequest req) {
  ASSERT(!req.err);

  for (uint64_t i = 0; i < req.headers.len; i++) {
    HttpHeader h = req.headers.data[i];

    if (!slice_eq(S("Content-Length"), h.key)) {
      continue;
    }

    return slice_parse_u64_decimal(h.value);
  }
  return (ParseNumberResult){0};
}

MUST_USE static HttpRequest request_read_body(HttpRequest req, Reader *reader,
                                              uint64_t content_length,
                                              Arena *arena) {
  ASSERT(!req.err);
  HttpRequest res = req;

  IoOperationResult io = reader_read_exactly(reader, content_length, arena);
  if (io.err) {
    res.err = io.err;
    return res;
  }

  res.body = io.slice;

  return res;
}

MUST_USE static HttpRequest request_read(Reader *reader, Arena *arena) {
  const LineRead status_line = reader_read_line(reader, arena);
  if (status_line.err) {
    return (HttpRequest){.err = status_line.err};
  }

  HttpRequest req = request_parse_status_line(status_line);
  if (req.err) {
    return req;
  }

  req = request_read_headers(req, reader, arena);
  if (req.err) {
    return req;
  }

  ParseNumberResult content_length = request_parse_content_length_maybe(req);
  if (content_length.err) {
    req.err = HS_ERR_INVALID_HTTP_REQUEST;
    return req;
  }

  if (content_length.present) {
    req = request_read_body(req, reader, content_length.n, arena);
  }

  return req;
}

MUST_USE static int response_write(Writer writer, HttpResponse res,
                                   Arena *arena) {
  // Invalid to both want to serve a file and a body.
  ASSERT(NULL == res.file_path || slice_is_empty(res.body));

  DynArrayU8 sb = {0};

  dyn_array_u8_append_cstr(&sb, "HTTP/1.1 ", arena);
  dyn_array_u8_append_u64(&sb, res.status, arena);
  dyn_array_u8_append_cstr(&sb, "\r\n", arena);

  for (uint64_t i = 0; i < res.headers.len; i++) {
    HttpHeader header = res.headers.data[i];
    dyn_append_slice(&sb, header.key, arena);
    dyn_array_u8_append_cstr(&sb, ": ", arena);
    dyn_append_slice(&sb, header.value, arena);
    dyn_array_u8_append_cstr(&sb, "\r\n", arena);
  }

  dyn_array_u8_append_cstr(&sb, "\r\n", arena);
  if (!slice_is_empty(res.body)) {
    dyn_append_slice(&sb, res.body, arena);
  }

  const Slice slice = dyn_array_u8_to_slice(sb);

  int err = writer_write_all(writer, slice);
  if (0 != err) {
    return err;
  }

  if (NULL != res.file_path) {
    int file_fd = open(res.file_path, O_RDONLY);
    if (file_fd == -1) {
      return errno;
    }

    struct stat st = {0};
    if (-1 == stat(res.file_path, &st)) {
      return errno;
    }

    ssize_t sent = os_sendfile(file_fd, (int)(uint64_t)writer.ctx, st.st_size);
    if (-1 == sent) {
      return errno;
    }
    if (sent != st.st_size) {
      // TODO: Retry.
      return ERANGE;
    }
  }

  return 0;
}

static void http_response_push_header(HttpResponse *res, Slice key, Slice value,
                                      Arena *arena) {
  *dyn_push(&res->headers, arena) = (HttpHeader){.key = key, .value = value};
}

static void http_response_push_header_cstr(HttpResponse *res, char *key,
                                           char *value, Arena *arena) {
  http_response_push_header(res, S(key), S(value), arena);
}

static void http_response_register_file_for_sending(HttpResponse *res,
                                                    char *path) {
  res->file_path = path;
}

typedef HttpResponse (*HttpRequestHandleFn)(HttpRequest req, Arena *arena);

static void handle_client(int socket, HttpRequestHandleFn handle) {
  struct timespec ts_start = {0};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts_start);

  Arena arena = arena_make_from_virtual_mem(CLIENT_MEM);
  Reader reader = reader_make_from_socket(socket);
  const HttpRequest req = request_read(&reader, &arena);

  log("http_request_start", arena, LCS("path", req.path),
      LCI("body_length", req.body.len), LCI("err", req.err),
      LCS("body", req.body), LCII("request_id", req.id),
      LCS("method", http_method_to_s(req.method)));
  if (req.err) {
    exit(EINVAL);
  }

  HttpResponse res = handle(req, &arena);

  Writer writer = writer_make_from_socket(socket);
  (void)response_write(writer, res, &arena);

  struct timespec ts_end = {0};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts_end);

  const uint64_t mem_use = CLIENT_MEM - (arena.end - arena.start);
  const uint64_t duration_us = (ts_end.tv_sec - ts_start.tv_sec) * 1000 * 1000 +
                               (ts_end.tv_nsec - ts_start.tv_nsec) / 1000;
  log("http_request_end", arena, LCI("arena_use", mem_use),
      LCI("duration_us", duration_us), LCS("path", req.path),
      LCS("method", http_method_to_s(req.method)));
}

MUST_USE static int run(HttpRequestHandleFn request_handler) {
  const uint16_t port = PORT;
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

  if ((err = listen(sock_fd, LISTEN_BACKLOG)) == -1) {
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
      handle_client(conn_fd, request_handler);
      exit(0);
    } else { // Parent
      // Fds are duplicated by fork(2) and need to be
      // closed by both parent & child
      close(conn_fd);
    }
  }
}
