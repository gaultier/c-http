#pragma once

#include "lib.c"

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint64_t HTTP_REQUEST_LINES_MAX_COUNT = 512;
static const uint64_t HTTP_SERVER_HANDLER_MEM_LEN = 8192;
static const uint16_t HTTP_SERVER_DEFAULT_PORT = 12345;
static const int TCP_LISTEN_BACKLOG = 16384;

typedef enum {
  HS_ERR_INVALID_HTTP_REQUEST,
  HS_ERR_INVALID_HTTP_RESPONSE,
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
  Error err;
} HttpRequest;

typedef struct {
  uint16_t status;
  DynArrayHttpHeaders headers;
  Error err;
  // TODO: Slice?
  char *file_path;
  Slice body;
} HttpResponse;

typedef struct {
  Slice slice;
  Error err;
} IoOperationResult;

typedef IoOperationResult (*ReadFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  uint64_t buf_idx;
  DynArrayU8 buf;
  void *ctx;
  ReadFn read_fn;
} Reader;

#define READER_IO_BUF_LEN 4096

[[nodiscard]] static IoOperationResult
reader_read_from_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_read = recv((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_read == -1) {
    return (IoOperationResult){.err = (Error)errno};
  }
  ASSERT(n_read >= 0);

  return (IoOperationResult){.slice.data = buf, .slice.len = (uint64_t)n_read};
}

[[nodiscard]] static Reader reader_make_from_socket(int socket) {
  return (Reader){
      .ctx = (void *)(uint64_t)socket,
      .read_fn = reader_read_from_socket,
  };
}

typedef IoOperationResult (*WriteFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  WriteFn write;
  void *ctx;
} Writer;

[[nodiscard]] static IoOperationResult
writer_write_from_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_written = send((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_written == -1) {
    return (IoOperationResult){.err = (Error)errno};
  }
  ASSERT(n_written >= 0);

  return (IoOperationResult){.slice.data = (uint8_t *)buf,
                             .slice.len = (uint64_t)n_written};
}

[[nodiscard]] static Writer writer_make_from_socket(int socket) {
  return (Writer){
      .ctx = (void *)(uint64_t)socket,
      .write = writer_write_from_socket,
  };
}

[[nodiscard]] static Error writer_write_all(Writer writer, Slice slice) {
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
  Error err;
  bool present;
} LineRead;

[[nodiscard]] static IoOperationResult reader_read_from_buffer(Reader *reader) {
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

[[nodiscard]] static IoOperationResult _reader_read_from_io(Reader *reader,
                                                            Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  uint8_t tmp[READER_IO_BUF_LEN] = {0};
  IoOperationResult res = reader->read_fn(reader->ctx, tmp, sizeof(tmp));
  if (res.err) {
    return res;
  }
  if (0 == res.slice.len) {
    return res;
  }

  Slice slice = {.data = tmp, .len = res.slice.len};

  uint64_t reader_buf_len_prev = reader->buf.len;
  dyn_append_slice(&reader->buf, slice, arena);

  res.slice.data = dyn_at_ptr(&reader->buf, reader_buf_len_prev);

  return res;
}

[[nodiscard]] static IoOperationResult reader_read(Reader *reader,
                                                   Arena *arena) {
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

[[nodiscard]] static IoOperationResult
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
    ASSERT(idx >= 0);

    // Found but maybe read some in excess, need to rewind a bit.
    uint64_t excess_read = io.slice.len - ((uint64_t)idx + needle.len);
    ASSERT(reader->buf_idx >= excess_read);
    reader->buf_idx -= excess_read;
    io.slice.len = (uint64_t)idx;
    return io;
  }

  io.err = EINTR;
  return io;
}

[[nodiscard]] static IoOperationResult
reader_read_exactly(Reader *reader, uint64_t content_length, Arena *arena) {
  uint64_t remaining_to_read = content_length;

  dyn_ensure_cap(&reader->buf, content_length, arena);
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

[[nodiscard]] static LineRead reader_read_line(Reader *reader, Arena *arena) {
  const Slice NEWLINE = S("\r\n");

  LineRead line = {0};

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

[[nodiscard]] static HttpRequest
request_parse_status_line(LineRead status_line) {
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

[[nodiscard]] static Error reader_read_headers(Reader *reader,
                                               DynArrayHttpHeaders *headers,
                                               Arena *arena) {
  dyn_ensure_cap(headers, 30, arena);

  for (uint64_t _i = 0; _i < HTTP_REQUEST_LINES_MAX_COUNT; _i++) {
    const LineRead line = reader_read_line(reader, arena);

    if (line.err) {
      return line.err;
    }

    if (!line.present || slice_is_empty(line.line)) {
      break;
    }

    SplitIterator it = slice_split_it(line.line, ':');
    SplitResult key = slice_split_next(&it);
    if (!key.ok) {
      return HS_ERR_INVALID_HTTP_REQUEST;
    }

    Slice key_trimmed = slice_trim(key.slice, ' ');

    Slice value = it.slice; // Remainder.
    Slice value_trimmed = slice_trim(value, ' ');

    HttpHeader header = {.key = key_trimmed, .value = value_trimmed};
    *dyn_push(headers, arena) = header;
  }
  return 0;
}

[[nodiscard]] static IoOperationResult reader_read_until_end(Reader *reader,
                                                             Arena *arena) {
  IoOperationResult res = {0};

  uint64_t reader_initial_idx = reader->buf_idx;

  for (;;) { // TODO: Bound?
    IoOperationResult io = reader_read(reader, arena);
    if (io.err) {
      res.err = io.err;
      // TODO: Set `res.slice` in this case?

      return res;
    }

    // First read?
    if (nullptr == res.slice.data) {
      res.slice.data = io.slice.data;
    }

    ASSERT(false == ckd_add(&res.slice.len, res.slice.len, io.slice.len));

    // End?
    if (0 == io.slice.len) {
      ASSERT(reader->buf_idx >= reader_initial_idx);
      ASSERT(reader->buf_idx == reader->buf.len);
      ASSERT(res.slice.len == reader->buf_idx - reader_initial_idx);

      if (0 != res.slice.len) {
        ASSERT(nullptr != res.slice.data);
      }

      return res;
    }
  }
}

[[nodiscard]] static ParseNumberResult
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

[[nodiscard]] static HttpRequest request_read_body(HttpRequest req,
                                                   Reader *reader,
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

[[nodiscard]] static HttpRequest request_read(Reader *reader, Arena *arena) {
  const LineRead status_line = reader_read_line(reader, arena);
  if (status_line.err) {
    return (HttpRequest){.err = status_line.err};
  }

  HttpRequest req = request_parse_status_line(status_line);
  if (req.err) {
    return req;
  }

  req.err = reader_read_headers(reader, &req.headers, arena);
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

[[nodiscard]] static Error response_write(Writer writer, HttpResponse res,
                                          Arena *arena) {
  // Invalid to both want to serve a file and a body.
  ASSERT(nullptr == res.file_path || slice_is_empty(res.body));

  DynArrayU8 sb = {0};

  dyn_array_u8_append_cstr(&sb, "HTTP/1.1 ", arena);
  dyn_array_u8_append_u64(&sb, res.status, arena);
  dyn_array_u8_append_cstr(&sb, "\r\n", arena);

  for (uint64_t i = 0; i < res.headers.len; i++) {
    HttpHeader header = dyn_at(res.headers, i);
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

  Error err = writer_write_all(writer, slice);
  if (0 != err) {
    return err;
  }

  if (nullptr != res.file_path) {
    int file_fd = open(res.file_path, O_RDONLY);
    if (file_fd == -1) {
      return (Error)errno;
    }

    struct stat st = {0};
    if (-1 == stat(res.file_path, &st)) {
      return (Error)errno;
    }

    ASSERT(st.st_size >= 0);

    err = os_sendfile(file_fd, (int)(uint64_t)writer.ctx, (uint64_t)st.st_size);
    if (err) {
      return err;
    }
  }

  return 0;
}

static void http_push_header(DynArrayHttpHeaders *headers, Slice key,
                             Slice value, Arena *arena) {
  *dyn_push(headers, arena) = (HttpHeader){.key = key, .value = value};
}

static void http_push_header_cstr(DynArrayHttpHeaders *headers, char *key,
                                  char *value, Arena *arena) {
  http_push_header(headers, S(key), S(value), arena);
}

static void http_response_register_file_for_sending(HttpResponse *res,
                                                    char *path) {
  res->file_path = path;
}

typedef HttpResponse (*HttpRequestHandleFn)(HttpRequest req, Arena *arena);

static void handle_client(int socket, HttpRequestHandleFn handle) {
  Arena arena = arena_make_from_virtual_mem(HTTP_SERVER_HANDLER_MEM_LEN);
  Reader reader = reader_make_from_socket(socket);
  const HttpRequest req = request_read(&reader, &arena);

  log(LOG_LEVEL_INFO, "http request start", arena, LCS("path", req.path),
      LCI("body_length", req.body.len), LCI("err", req.err),
      LCII("request_id", req.id), LCS("method", http_method_to_s(req.method)));
  if (req.err) {
    log(LOG_LEVEL_ERROR, "http request read", arena, LCI("err", req.err),
        LCII("request_id", req.id));
    exit(EINVAL);
  }

  HttpResponse res = handle(req, &arena);

  Writer writer = writer_make_from_socket(socket);
  Error err = response_write(writer, res, &arena);
  if (err) {
    log(LOG_LEVEL_ERROR, "http request write", arena, LCI("err", err),
        LCII("request_id", req.id));
  }

  ASSERT(arena.end >= arena.start);

  const uint64_t mem_use = HTTP_SERVER_HANDLER_MEM_LEN -
                           ((uint64_t)arena.end - (uint64_t)arena.start);
  log(LOG_LEVEL_INFO, "http request end", arena, LCI("arena_use", mem_use),
      LCS("path", req.path), LCI("header_count", req.headers.len),
      LCI("status", res.status), LCS("method", http_method_to_s(req.method)),
      LCS("res.file_path", S(res.file_path)), LCI("res.body.len", res.body.len),
      LCII("request_id", req.id));
}

static Error http_server_run(uint16_t port, HttpRequestHandleFn request_handler,
                             Arena *arena) {

  struct sigaction sa = {.sa_flags = SA_NOCLDWAIT};
  if (-1 == sigaction(SIGCHLD, &sa, nullptr)) {
    log(LOG_LEVEL_ERROR, "sigaction(2)", *arena, LCI("err", (uint64_t)errno));
    return (Error)errno;
  }

  const int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == sock_fd) {
    log(LOG_LEVEL_ERROR, "socket(2)", *arena, LCI("err", (uint64_t)errno));
    return (Error)errno;
  }

  int val = 1;
  if (-1 == setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val))) {
    log(LOG_LEVEL_ERROR, "setsockopt(2)", *arena, LCI("err", (uint64_t)errno),
        LCS("option", S("SO_REUSEADDR")));
    return (Error)errno;
  }

#ifdef __FreeBSD__
  if (-1 == setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val))) {
    log(LOG_LEVEL_ERROR, "setsockopt(2)", *arena, LCI("err", (uint64_t)errno),
        LCS("option", S("SO_REUSEPORT")));
    return (Error)errno;
  }
#endif

  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };

  if (-1 == bind(sock_fd, (const struct sockaddr *)&addr, sizeof(addr))) {
    log(LOG_LEVEL_ERROR, "bind(2)", *arena, LCI("err", (uint64_t)errno));
    return (Error)errno;
  }

  if (-1 == listen(sock_fd, TCP_LISTEN_BACKLOG)) {
    log(LOG_LEVEL_ERROR, "listen(2)", *arena, LCI("err", (uint64_t)errno));
    return (Error)errno;
  }

  log(LOG_LEVEL_INFO, "http server listening", *arena, LCI("port", port),
      LCI("backlog", TCP_LISTEN_BACKLOG));

  while (true) {
    // TODO: Should we have a thread dedicated to `accept` and a thread
    // dedicated to `wait()`-ing on children processes, to cap the number of
    // concurrent requests being served?
    // Currently it is boundless.
    const int conn_fd = accept(sock_fd, nullptr, 0);
    if (conn_fd == -1) {
      log(LOG_LEVEL_ERROR, "accept(2)", *arena, LCI("err", (uint64_t)errno),
          LCI("arena.available",
              (uint64_t)arena->end - (uint64_t)arena->start));
      if (EINTR == errno) {
        continue;
      }
      return (Error)errno;
    }

    const pid_t pid = fork();
    if (pid == -1) {
      log(LOG_LEVEL_ERROR, "fork(2)", *arena, LCI("err", (uint64_t)errno));
      return (Error)errno;
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

[[maybe_unused]] [[nodiscard]] static HttpResponse
http_client_request(struct sockaddr *addr, uint32_t addr_sizeof,
                    HttpRequest req, Arena *arena) {
  HttpResponse res = {0};
  if (HM_UNKNOWN == req.method) {
    res.err = EINVAL;
    return res;
  }

  int client = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == client) {
    res.err = (Error)errno;
    return res;
  }

  if (-1 == connect(client, addr, addr_sizeof)) {
    res.err = (Error)errno;
    goto end;
  }

  DynArrayU8 sb = {0};
  dyn_append_slice(&sb, http_method_to_s(req.method), arena);
  dyn_append_slice(&sb, S(" "), arena);
  dyn_append_slice(&sb, req.path, arena);
  dyn_append_slice(&sb, S(" HTTP/1.1"), arena);
  dyn_append_slice(&sb, S("\r\n"), arena);

  for (uint64_t i = 0; i < req.headers.len; i++) {
    HttpHeader header = dyn_at(req.headers, i);
    dyn_append_slice(&sb, header.key, arena);
    dyn_append_slice(&sb, S(": "), arena);
    dyn_append_slice(&sb, header.value, arena);
    dyn_append_slice(&sb, S("\r\n"), arena);
  }
  dyn_append_slice(&sb, S("\r\n"), arena);
  dyn_append_slice(&sb, req.body, arena);

  ASSERT(send(client, sb.data, sb.len, 0) == (int64_t)sb.len);

  Reader reader = reader_make_from_socket(client);

  {
    LineRead status_line = reader_read_line(&reader, arena);
    if (status_line.err) {
      res.err = status_line.err;
      goto end;
    }
    if (!status_line.present) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    Slice http_version_needle = S("HTTP/1.1 ");
    if (!slice_starts_with(status_line.line, http_version_needle)) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    Slice status_str =
        slice_range(status_line.line, http_version_needle.len, 0);
    ParseNumberResult status_parsed = slice_parse_u64_decimal(status_str);
    if (status_parsed.err) {
      res.err = status_parsed.err;
      goto end;
    }
    if (!status_parsed.present) {
      res.err = EINVAL;
      goto end;
    }
    if (!(200 <= status_parsed.n && status_parsed.n <= 599)) {
      res.err = EINVAL;
      goto end;
    }

    res.status = (uint16_t)status_parsed.n;
  }

  res.err = reader_read_headers(&reader, &res.headers, arena);
  if (res.err) {
    goto end;
  }

  // Read body.
  IoOperationResult body = reader_read_until_end(&reader, arena);
  if (body.err) {
    res.err = body.err;
    goto end;
  }

  res.body = body.slice;

end:
  close(client);
  return res;
}
