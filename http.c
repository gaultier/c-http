#ifndef CHTTP_HTTP_C
#define CHTTP_HTTP_C

#include "submodules/cstd/lib.c"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const u64 HTTP_REQUEST_LINES_MAX_COUNT = 512;
static const u64 HTTP_SERVER_HANDLER_MEM_LEN = 12 * KiB;
[[maybe_unused]]
static const u16 HTTP_SERVER_DEFAULT_PORT = 12345;
static const int TCP_LISTEN_BACKLOG = 16384;

typedef enum { HM_UNKNOWN, HM_GET, HM_POST } HttpMethod;

String static http_method_to_s(HttpMethod m) {
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
  String key, value;
} KeyValue;

typedef struct {
  KeyValue *data;
  u64 len, cap;
} DynKeyValue;

typedef struct {
  String id;
  String path_raw;
  DynString path_components;
  DynKeyValue url_parameters;
  HttpMethod method;
  DynKeyValue headers;
  String body;
  Error err;
} HttpRequest;

typedef struct {
  u16 status;
  DynKeyValue headers;
  Error err;

  // TODO: union{file_path,body}?
  String file_path;
  String body;
} HttpResponse;

typedef struct {
  String s;
  Error err;
} IoOperationResult;

typedef IoOperationResult (*ReadFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  u64 buf_idx;
  DynU8 buf;
  void *ctx;
  ReadFn read_fn;
} Reader;

#define READER_IO_BUF_LEN (4 * KiB)

[[nodiscard]] static IoOperationResult
reader_read_from_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_read = recv((int)(u64)ctx, buf, buf_len, 0);
  if (n_read == -1) {
    return (IoOperationResult){.err = (Error)errno};
  }
  ASSERT(n_read >= 0);

  return (IoOperationResult){.s.data = buf, .s.len = (u64)n_read};
}

[[nodiscard]] static Reader reader_make_from_socket(int socket) {
  return (Reader){
      .ctx = (void *)(u64)socket,
      .read_fn = reader_read_from_socket,
  };
}

typedef IoOperationResult (*WriteFn)(void *ctx, void *buf, size_t buf_len);
typedef void (*CloseFn)(void *ctx);

typedef struct {
  WriteFn write;
  CloseFn close;
  void *ctx;
} Writer;

[[maybe_unused]]
static void writer_close_socket(void *ctx) {
  int socket_fd = (int)(u64)ctx;
  if (socket_fd > 0) {
    close(socket_fd);
  }
}

[[maybe_unused]]
static void writer_close(Writer *writer) {
  if (writer->close) {
    writer->close(writer->ctx);
  }
}

[[nodiscard]] static IoOperationResult
writer_write_to_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_written = send((int)(u64)ctx, buf, buf_len, 0);
  if (n_written == -1) {
    return (IoOperationResult){.err = (Error)errno};
  }
  ASSERT(n_written >= 0);

  return (IoOperationResult){.s.data = (u8 *)buf, .s.len = (u64)n_written};
}

[[nodiscard]] static Writer writer_make_from_socket(int socket) {
  return (Writer){
      .ctx = (void *)(u64)socket,
      .write = writer_write_to_socket,
      .close = writer_close_socket,
  };
}

[[nodiscard]] static Error writer_write_all(Writer writer, String s) {
  for (u64 idx = 0; idx < s.len;) {
    const String to_write = slice_range(s, idx, 0);
    const IoOperationResult write_res =
        writer.write(writer.ctx, to_write.data, to_write.len);
    if (write_res.err) {
      return write_res.err;
    }

    ASSERT(write_res.s.len <= s.len);
    ASSERT(idx <= s.len);
    idx += write_res.s.len;
    ASSERT(idx <= s.len);
  }
  return 0;
}

typedef struct {
  DynU8 sb;
  Arena *arena;
} WriterBufCtx;

[[nodiscard]] static IoOperationResult
writer_write_to_buf(void *ctx_any, void *buf, size_t buf_len) {
  WriterBufCtx *ctx = ctx_any;
  String src = {.data = buf, .len = buf_len};
  dyn_append_slice(&ctx->sb, src, ctx->arena);

  return (IoOperationResult){.s = src};
}

[[maybe_unused]] [[nodiscard]] static Writer writer_make_for_buf(Arena *arena) {
  WriterBufCtx *ctx = arena_new(arena, WriterBufCtx, 1);
  ctx->arena = arena;

  return (Writer){
      .ctx = ctx,
      .write = writer_write_to_buf,
  };
}

typedef struct {
  String line;
  Error err;
  bool present;
} LineRead;

[[nodiscard]] static IoOperationResult reader_read_from_buffer(Reader *reader) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  if (reader->buf_idx == reader->buf.len) {
    return (IoOperationResult){0};
  }

  IoOperationResult res = {
      .s.data = &reader->buf.data[reader->buf_idx],
      .s.len = reader->buf.len - reader->buf_idx,
  };
  reader->buf_idx = reader->buf.len;

  return res;
}

[[nodiscard]] static IoOperationResult _reader_read_from_io(Reader *reader,
                                                            Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  u8 tmp[READER_IO_BUF_LEN] = {0};
  IoOperationResult res = reader->read_fn(reader->ctx, tmp, sizeof(tmp));
  if (res.err) {
    return res;
  }
  if (0 == res.s.len) {
    return res;
  }

  String s = {.data = tmp, .len = res.s.len};

  u64 reader_buf_len_prev = reader->buf.len;
  dyn_append_slice(&reader->buf, s, arena);

  res.s.data = dyn_at_ptr(&reader->buf, reader_buf_len_prev);

  return res;
}

[[nodiscard]] static IoOperationResult reader_read(Reader *reader,
                                                   Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  IoOperationResult res = reader_read_from_buffer(reader);
  if (res.err || !slice_is_empty(res.s)) {
    return res;
  }

  res = _reader_read_from_io(reader, arena);
  if (res.err) {
    return res;
  }

  return reader_read_from_buffer(reader);
}

[[nodiscard]] static IoOperationResult
reader_read_until_slice(Reader *reader, String needle, Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  IoOperationResult io = {0};

  for (u64 i = 0; i < 128; i++) // FIXME
  {
    io = reader_read(reader, arena);
    if (io.err) {
      return io;
    }

    i64 idx = string_indexof_string(io.s, needle);
    // Not found, continue reading.
    if (idx == -1) {
      continue;
    }
    ASSERT(idx >= 0);

    // Found but maybe read some in excess, need to rewind a bit.
    u64 excess_read = io.s.len - ((u64)idx + needle.len);
    ASSERT(reader->buf_idx >= excess_read);
    reader->buf_idx -= excess_read;
    io.s.len = (u64)idx;
    return io;
  }

  io.err = EINTR;
  return io;
}

[[nodiscard]] static IoOperationResult
reader_read_up_to(Reader *reader, u64 count, Arena *arena) {
  dyn_ensure_cap(&reader->buf, count, arena);

  IoOperationResult res =
      reader->read_fn(reader->ctx, &reader->buf.data[reader->buf.len], count);
  return res;
}

[[nodiscard]] static IoOperationResult
reader_read_exactly(Reader *reader, u64 count, Arena *arena) {
  u64 remaining_to_read = count;

  dyn_ensure_cap(&reader->buf, count, arena);
  IoOperationResult res = {0};

  for (u64 i = 0; i < count; i++) {
    if (0 == remaining_to_read) {
      ASSERT(res.s.len == count);
      return res;
    }

    ASSERT(remaining_to_read > 0);

    res = reader_read_up_to(reader, remaining_to_read, arena);
    if (res.err) {
      return res;
    }

    ASSERT(res.s.len <= remaining_to_read);
    remaining_to_read -= res.s.len;
  }
  return res;
}

[[nodiscard]] static LineRead reader_read_line(Reader *reader, Arena *arena) {
  const String NEWLINE = S("\r\n");

  LineRead line = {0};

  const IoOperationResult io_result =
      reader_read_until_slice(reader, NEWLINE, arena);
  if (io_result.err) {
    line.err = io_result.err;
    return line;
  }

  line.present = true;
  line.line = io_result.s;
  return line;
}

[[nodiscard]] static DynString
http_parse_relative_path(String s, bool must_start_with_slash, Arena *arena) {
  if (must_start_with_slash) {
    ASSERT(string_starts_with(s, S("/")));
  }

  DynString res = {0};

  SplitIterator split_it_question = string_split(s, '?');
  String work = string_split_next(&split_it_question).s;

  SplitIterator split_it_slash = string_split(work, '/');
  for (u64 i = 0; i < s.len; i++) { // Bound.
    SplitResult split = string_split_next(&split_it_slash);
    if (!split.ok) {
      break;
    }

    if (slice_is_empty(split.s)) {
      continue;
    }

    *dyn_push(&res, arena) = split.s;
  }

  return res;
}

[[nodiscard]] static String make_unique_id_u128_string(Arena *arena) {
  u128 id = 0;
  arc4random_buf(&id, sizeof(id));

  DynU8 dyn = {0};
  dynu8_append_u128_hex(&dyn, id, arena);

  return dyn_slice(String, dyn);
}

[[nodiscard]] static HttpRequest request_parse_status_line(LineRead status_line,
                                                           Arena *arena) {
  HttpRequest req = {.id = make_unique_id_u128_string(arena)};

  if (!status_line.present) {
    req.err = HS_ERR_INVALID_HTTP_REQUEST;
    return req;
  }
  if (status_line.err) {
    req.err = status_line.err;
    return req;
  }

  SplitIterator it = string_split(status_line.line, ' ');

  {
    SplitResult method = string_split_next(&it);
    if (!method.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (string_eq(method.s, S("GET"))) {
      req.method = HM_GET;
    } else if (string_eq(method.s, S("POST"))) {
      req.method = HM_POST;
    } else {
      // FIXME: More.
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }
  }

  {
    SplitResult path = string_split_next(&it);
    if (!path.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (slice_is_empty(path.s)) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (path.s.data[0] != '/') {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    req.path_raw = path.s;
    req.path_components = http_parse_relative_path(path.s, true, arena);
  }

  {
    SplitResult http_version = string_split_next(&it);
    if (!http_version.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (!string_eq(http_version.s, S("HTTP/1.1"))) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }
  }

  return req;
}

[[nodiscard]] static Error
reader_read_headers(Reader *reader, DynKeyValue *headers, Arena *arena) {
  dyn_ensure_cap(headers, 30, arena);

  for (u64 _i = 0; _i < HTTP_REQUEST_LINES_MAX_COUNT; _i++) {
    const LineRead line = reader_read_line(reader, arena);

    if (line.err) {
      return line.err;
    }

    if (!line.present || slice_is_empty(line.line)) {
      break;
    }

    SplitIterator it = string_split(line.line, ':');
    SplitResult key = string_split_next(&it);
    if (!key.ok) {
      return HS_ERR_INVALID_HTTP_REQUEST;
    }

    String key_trimmed = string_trim(key.s, ' ');

    String value = it.s; // Remainder.
    String value_trimmed = string_trim(value, ' ');

    KeyValue header = {.key = key_trimmed, .value = value_trimmed};
    *dyn_push(headers, arena) = header;
  }
  return 0;
}

[[nodiscard]] static IoOperationResult reader_read_until_end(Reader *reader,
                                                             Arena *arena) {
  IoOperationResult res = {0};

  u64 reader_initial_idx = reader->buf_idx;

  for (;;) { // TODO: Bound?
    IoOperationResult io = reader_read(reader, arena);
    if (io.err) {
      res.err = io.err;
      // TODO: Set `res.s` in this case?

      return res;
    }

    // First read?
    if (nullptr == res.s.data) {
      res.s.data = io.s.data;
    }

    ASSERT(false == ckd_add(&res.s.len, res.s.len, io.s.len));

    // End?
    if (0 == io.s.len) {
      ASSERT(reader->buf_idx >= reader_initial_idx);
      ASSERT(reader->buf_idx == reader->buf.len);
      ASSERT(res.s.len == reader->buf_idx - reader_initial_idx);

      if (0 != res.s.len) {
        ASSERT(nullptr != res.s.data);
      }

      return res;
    }
  }
}

[[nodiscard]] static ParseNumberResult
request_parse_content_length_maybe(HttpRequest req, Arena *arena) {
  ASSERT(!req.err);

  for (u64 i = 0; i < req.headers.len; i++) {
    KeyValue h = req.headers.data[i];

    if (!string_ieq_ascii(S("Content-Length"), h.key, arena)) {
      continue;
    }

    return string_parse_u64(h.value);
  }
  return (ParseNumberResult){0};
}

[[nodiscard]] static HttpRequest request_read_body(HttpRequest req,
                                                   Reader *reader,
                                                   u64 content_length,
                                                   Arena *arena) {
  ASSERT(!req.err);
  HttpRequest res = req;

  IoOperationResult io = reader_read_exactly(reader, content_length, arena);
  if (io.err) {
    res.err = io.err;
    return res;
  }

  res.body = io.s;

  return res;
}

[[nodiscard]] static HttpRequest request_read(Reader *reader, Arena *arena) {
  const LineRead status_line = reader_read_line(reader, arena);
  if (status_line.err) {
    return (HttpRequest){.err = status_line.err};
  }

  HttpRequest req = request_parse_status_line(status_line, arena);
  if (req.err) {
    return req;
  }

  req.err = reader_read_headers(reader, &req.headers, arena);
  if (req.err) {
    return req;
  }

  ParseNumberResult content_length =
      request_parse_content_length_maybe(req, arena);
  if (!content_length.present) {
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
  ASSERT(slice_is_empty(res.file_path) || slice_is_empty(res.body));

  DynU8 sb = {0};

  dyn_append_slice(&sb, S("HTTP/1.1 "), arena);
  dynu8_append_u64(&sb, res.status, arena);
  dyn_append_slice(&sb, S("\r\n"), arena);

  for (u64 i = 0; i < res.headers.len; i++) {
    KeyValue header = dyn_at(res.headers, i);
    dyn_append_slice(&sb, header.key, arena);
    dyn_append_slice(&sb, S(": "), arena);
    dyn_append_slice(&sb, header.value, arena);
    dyn_append_slice(&sb, S("\r\n"), arena);
  }

  dyn_append_slice(&sb, S("\r\n"), arena);
  if (!slice_is_empty(res.body)) {
    dyn_append_slice(&sb, res.body, arena);
  }

  const String s = dyn_slice(String, sb);

  Error err = writer_write_all(writer, s);
  if (0 != err) {
    return err;
  }

  if (!slice_is_empty(res.file_path)) {
    char *file_path_c = string_to_cstr(res.file_path, arena);
    int file_fd = open(file_path_c, O_RDONLY);
    if (file_fd == -1) {
      return (Error)errno;
    }

    struct stat st = {0};
    if (-1 == stat(file_path_c, &st)) {
      return (Error)errno;
    }

    ASSERT(st.st_size >= 0);

    err = os_sendfile(file_fd, (int)(u64)writer.ctx, (u64)st.st_size);
    if (err) {
      return err;
    }
  }

  return 0;
}

static void http_push_header(DynKeyValue *headers, String key, String value,
                             Arena *arena) {
  *dyn_push(headers, arena) = (KeyValue){.key = key, .value = value};
}

[[maybe_unused]] static void
http_response_register_file_for_sending(HttpResponse *res, String path) {
  ASSERT(!slice_is_empty(path));
  res->file_path = path;
}

typedef HttpResponse (*HttpRequestHandleFn)(HttpRequest req, void *ctx,
                                            Arena *arena);

static void handle_client(int socket, HttpRequestHandleFn handle, void *ctx) {
  Arena arena = arena_make_from_virtual_mem(HTTP_SERVER_HANDLER_MEM_LEN);
  Reader reader = reader_make_from_socket(socket);
  const HttpRequest req = request_read(&reader, &arena);

  log(LOG_LEVEL_INFO, "http request start", &arena, L("req.path", req.path_raw),
      L("req.body.len", req.body.len), L("err", req.err),
      L("req.headers.len", req.headers.len), L("req.id", req.id),
      L("req.method", http_method_to_s(req.method)));
  if (req.err) {
    log(LOG_LEVEL_ERROR, "http request read", &arena, L("err", req.err),
        L("req.id", req.id));
    return;
  }

  HttpResponse res = handle(req, ctx, &arena);
  http_push_header(&res.headers, S("Connection"), S("close"), &arena);

  Writer writer = writer_make_from_socket(socket);
  Error err = response_write(writer, res, &arena);
  if (err) {
    log(LOG_LEVEL_ERROR, "http request write", &arena, L("err", err),
        L("req.id", req.id));
  }

  ASSERT(arena.end >= arena.start);

  const u64 mem_use =
      HTTP_SERVER_HANDLER_MEM_LEN - ((u64)arena.end - (u64)arena.start);
  log(LOG_LEVEL_INFO, "http request end", &arena, L("arena_use", mem_use),
      L("req.path", req.path_raw), L("req.headers.len", req.headers.len),
      L("res.headers.len", res.headers.len), L("status", res.status),
      L("req.method", http_method_to_s(req.method)),
      L("res.file_path", res.file_path), L("res.body.len", res.body.len),
      L("req.id", req.id));

  close(socket);
}

[[maybe_unused]] [[nodiscard]]
static Error http_server_run(u16 port, HttpRequestHandleFn request_handler,
                             void *ctx, Arena *arena) {

  struct sigaction sa = {.sa_flags = SA_NOCLDWAIT};
  if (-1 == sigaction(SIGCHLD, &sa, nullptr)) {
    log(LOG_LEVEL_ERROR, "sigaction(2)", arena, L("err", errno));
    return (Error)errno;
  }

  const int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == sock_fd) {
    log(LOG_LEVEL_ERROR, "socket(2)", arena, L("err", errno));
    return (Error)errno;
  }

  int val = 1;
  if (-1 == setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val))) {
    log(LOG_LEVEL_ERROR, "setsockopt(2)", arena, L("err", errno),
        L("option", S("SO_REUSEADDR")));
    return (Error)errno;
  }

#ifdef __FreeBSD__
  if (-1 == setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val))) {
    log(LOG_LEVEL_ERROR, "setsockopt(2)", arena, L("err", errno),
        L("option", S("SO_REUSEPORT")));
    return (Error)errno;
  }
#endif

  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };

  if (-1 == bind(sock_fd, (const struct sockaddr *)&addr, sizeof(addr))) {
    log(LOG_LEVEL_ERROR, "bind(2)", arena, L("err", errno));
    return (Error)errno;
  }

  if (-1 == listen(sock_fd, TCP_LISTEN_BACKLOG)) {
    log(LOG_LEVEL_ERROR, "listen(2)", arena, L("err", errno));
    return (Error)errno;
  }

  log(LOG_LEVEL_INFO, "http server listening", arena, L("port", port),
      L("backlog", TCP_LISTEN_BACKLOG));

  while (true) {
    // TODO: setrlimit(2) to cap the number of child processes.
    const int conn_fd = accept(sock_fd, nullptr, 0);
    if (conn_fd == -1) {
      log(LOG_LEVEL_ERROR, "accept(2)", arena, L("err", errno),
          L("arena.available", (u64)arena->end - (u64)arena->start));
      if (EINTR == errno) {
        continue;
      }
      return (Error)errno;
    }

    pid_t pid = fork();
    if (pid == -1) { // Error.
      log(LOG_LEVEL_ERROR, "fork(2)", arena, L("err", errno));
      close(conn_fd);
    } else if (pid == 0) { // Child.
      handle_client(conn_fd, request_handler, ctx);
      exit(0);
    } else { // Parent.
      close(conn_fd);
    }
  }
}

[[maybe_unused]] static void url_encode_string(DynU8 *sb, String key,
                                               String value, Arena *arena) {
  for (u64 i = 0; i < key.len; i++) {
    u8 c = slice_at(key, i);
    if (ch_is_alphanumeric(c)) {
      *dyn_push(sb, arena) = c;
    } else {
      *dyn_push(sb, arena) = '%';
      dynu8_append_u8_hex_upper(sb, c, arena);
    }
  }

  *dyn_push(sb, arena) = '=';

  for (u64 i = 0; i < value.len; i++) {
    u8 c = slice_at(value, i);
    if (ch_is_alphanumeric(c)) {
      *dyn_push(sb, arena) = c;
    } else {
      *dyn_push(sb, arena) = '%';
      dynu8_append_u8_hex_upper(sb, c, arena);
    }
  }
}

// TODO: Split serializing body?
[[nodiscard]] static String http_request_serialize(HttpRequest req,
                                                   Arena *arena) {
  DynU8 sb = {0};
  dyn_append_slice(&sb, http_method_to_s(req.method), arena);
  dyn_append_slice(&sb, S(" /"), arena);

  for (u64 i = 0; i < req.path_components.len; i++) {
    String path_component = dyn_at(req.path_components, i);
    dyn_append_slice(&sb, path_component, arena);

    if (i < req.path_components.len - 1) {
      *dyn_push(&sb, arena) = '/';
    }
  }

  if (req.url_parameters.len > 0) {
    *dyn_push(&sb, arena) = '?';
    for (u64 i = 0; i < req.url_parameters.len; i++) {
      KeyValue param = dyn_at(req.url_parameters, i);
      url_encode_string(&sb, param.key, param.value, arena);

      if (i < req.url_parameters.len - 1) {
        *dyn_push(&sb, arena) = '&';
      }
    }
  }

  dyn_append_slice(&sb, S(" HTTP/1.1"), arena);
  dyn_append_slice(&sb, S("\r\n"), arena);

  for (u64 i = 0; i < req.headers.len; i++) {
    KeyValue header = dyn_at(req.headers, i);
    dyn_append_slice(&sb, header.key, arena);
    dyn_append_slice(&sb, S(": "), arena);
    dyn_append_slice(&sb, header.value, arena);
    dyn_append_slice(&sb, S("\r\n"), arena);
  }
  dyn_append_slice(&sb, S("\r\n"), arena);
  dyn_append_slice(&sb, req.body, arena);

  return dyn_slice(String, sb);
}

[[maybe_unused]] [[nodiscard]] static HttpResponse
http_client_request(String host, u16 port, HttpRequest req, Arena *arena) {
  HttpResponse res = {0};

  if (!slice_is_empty(req.path_raw)) {
    // Should use `req.path_components`, not `path.raw`.
    res.err = EINVAL;
    return res;
  }

  if (HM_UNKNOWN == req.method) {
    res.err = EINVAL;
    return res;
  }

  struct addrinfo *result = nullptr, *rp = nullptr;
  struct addrinfo hints = {0};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  int res_getaddrinfo = getaddrinfo(
      string_to_cstr(host, arena),
      string_to_cstr(u64_to_string(port, arena), arena), &hints, &result);
  if (res_getaddrinfo != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res_getaddrinfo));
    res.status = EINVAL;
    return res;
  }

  /* getaddrinfo() returns a list of address structures.
     Try each address until we successfully connect(2).
     If socket(2) (or connect(2)) fails, we (close the socket
     and) try the next address. */

  int client_socket = 0;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    client_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (client_socket == -1)
      continue;

    if (connect(client_socket, rp->ai_addr, rp->ai_addrlen) == 0)
      break; /* Success */

    close(client_socket);
  }

  freeaddrinfo(result); /* No longer needed */

  if (rp == NULL) { /* No address succeeded */
    log(LOG_LEVEL_ERROR, "http request did not find any suitable ip/port",
        arena, L("req.method", req.method), L("req.path_raw", req.path_raw));
    res.err = EINVAL;
    return res;
  }

  String http_request_serialized = http_request_serialize(req, arena);

  // TODO: should not be an assert but a returned error.
  ASSERT(send(client_socket, http_request_serialized.data,
              http_request_serialized.len,
              0) == (i64)http_request_serialized.len);

  Reader reader = reader_make_from_socket(client_socket);

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

    String http1_1_version_needle = S("HTTP/1.1 ");
    String http1_0_version_needle = S("HTTP/1.0 ");
    ASSERT(http1_0_version_needle.len == http1_1_version_needle.len);

    if (!(string_starts_with(status_line.line, http1_0_version_needle) ||
          string_starts_with(status_line.line, http1_1_version_needle))) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    String status_str =
        slice_range(status_line.line, http1_1_version_needle.len, 0);
    ParseNumberResult status_parsed = string_parse_u64(status_str);
    if (!status_parsed.present) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
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

    res.status = (u16)status_parsed.n;
  }

  res.err = reader_read_headers(&reader, &res.headers, arena);
  if (res.err) {
    log(LOG_LEVEL_ERROR, "http request failed to read headers", arena,
        L("req.method", req.method), L("req.path_raw", req.path_raw),
        L("err", res.err));
    goto end;
  }

  // Read body.
  IoOperationResult body = reader_read_until_end(&reader, arena);
  if (body.err) {
    log(LOG_LEVEL_ERROR, "http request failed to read body", arena,
        L("req.method", req.method), L("req.path_raw", req.path_raw),
        L("err", body.err));
    res.err = body.err;
    goto end;
  }

  res.body = body.s;

end:
  close(client_socket);
  return res;
}

typedef struct {
  String key, value;
} FormDataKV;

typedef struct {
  FormDataKV *data;
  u64 len, cap;
} DynFormData;

typedef struct {
  // NOTE: Repeated keys are allowed, that's how 'arrays' are encoded.
  DynFormData form;
  Error err;
} FormDataParseResult;

typedef struct {
  FormDataKV kv;
  Error err;
  String remaining;
} FormDataKVParseResult;

typedef struct {
  String data;
  Error err;
  String remaining;
} FormDataKVElementParseResult;

[[nodiscard]] static FormDataKVElementParseResult
form_data_kv_parse_element(String in, u8 ch_terminator, Arena *arena) {
  FormDataKVElementParseResult res = {0};
  DynU8 data = {0};

  u64 i = 0;
  for (; i < in.len; i++) {
    u8 c = in.data[i];

    if ('+' == c) {
      *dyn_push(&data, arena) = ' ';
    } else if ('%' == c) {
      if ((in.len - i) < 2) {
        res.err = HS_ERR_INVALID_FORM_DATA;
        return res;
      }
      u8 c1 = in.data[i + 1];
      u8 c2 = in.data[i + 2];

      if (!(ch_is_hex_digit(c1) && ch_is_hex_digit(c2))) {
        res.err = HS_ERR_INVALID_FORM_DATA;
        return res;
      }

      u8 utf8_character = ch_from_hex(c1) * 16 + ch_from_hex(c2);
      *dyn_push(&data, arena) = utf8_character;
      i += 2; // Consume 2 characters.
    } else if (ch_terminator == c) {
      i += 1; // Consume.
      break;
    } else {
      *dyn_push(&data, arena) = c;
    }
  }

  res.data = dyn_slice(String, data);
  res.remaining = slice_range(in, i, 0);
  return res;
}

[[nodiscard]] static FormDataKVParseResult form_data_kv_parse(String in,
                                                              Arena *arena) {
  FormDataKVParseResult res = {0};

  String remaining = in;

  FormDataKVElementParseResult key_parsed =
      form_data_kv_parse_element(remaining, '=', arena);
  if (key_parsed.err) {
    res.err = key_parsed.err;
    return res;
  }
  res.kv.key = key_parsed.data;

  remaining = key_parsed.remaining;

  FormDataKVElementParseResult value_parsed =
      form_data_kv_parse_element(remaining, '&', arena);
  if (value_parsed.err) {
    res.err = value_parsed.err;
    return res;
  }
  res.kv.value = value_parsed.data;
  res.remaining = value_parsed.remaining;

  return res;
}

[[maybe_unused]] [[nodiscard]] static FormDataParseResult
form_data_parse(String in, Arena *arena) {
  FormDataParseResult res = {0};

  String remaining = in;

  for (u64 i = 0; i < in.len; i++) { // Bound.
    if (slice_is_empty(remaining)) {
      break;
    }

    FormDataKVParseResult kv = form_data_kv_parse(remaining, arena);
    if (kv.err) {
      res.err = kv.err;
      return res;
    }

    *dyn_push(&res.form, arena) = kv.kv;

    remaining = kv.remaining;
  }
  return res;
}

typedef enum {
  HTML_NONE,
  HTML_TITLE,
  HTML_SPAN,
  HTML_INPUT,
  HTML_BUTTON,
  HTML_LINK,
  HTML_META,
  HTML_HEAD,
  HTML_BODY,
  HTML_DIV,
  HTML_OL,
  HTML_LI,
  HTML_TEXT,
  HTML_FORM,
  HTML_FIELDSET,
  HTML_LABEL,
  HTML_SCRIPT,
  HTML_STYLE,
  HTML_LEGEND,
  HTML_MAX, // Pseudo.
} HtmlKind;

typedef struct HtmlElement HtmlElement;
typedef struct {
  HtmlElement *data;
  u64 len, cap;
} DynHtmlElements;

struct HtmlElement {
  HtmlKind kind;
  DynKeyValue attributes;
  union {
    DynHtmlElements children;
    String text; // Only for `HTML_TEXT`, `HTML_LEGEND`, `HTML_TITLE`,
                 // `HTML_SCRIPT`, `HTML_STYLE`, `HTML_BUTTON`.
  };
};

typedef struct {
  HtmlElement body;
  HtmlElement head;
} HtmlDocument;

[[maybe_unused]] [[nodiscard]] static HtmlDocument html_make(String title,
                                                             Arena *arena) {
  HtmlDocument res = {0};

  {

    HtmlElement tag_head = {.kind = HTML_HEAD};
    {
      HtmlElement tag_meta = {.kind = HTML_META};
      {
        *dyn_push(&tag_meta.attributes, arena) =
            (KeyValue){.key = S("charset"), .value = S("utf-8")};
      }
      *dyn_push(&tag_head.children, arena) = tag_meta;
    }
    {
      HtmlElement tag_title = {.kind = HTML_TITLE, .text = title};
      *dyn_push(&tag_head.children, arena) = tag_title;
    }

    res.head = tag_head;

    HtmlElement tag_body = {.kind = HTML_BODY};
    res.body = tag_body;
  }

  return res;
}

static void html_attributes_to_string(DynKeyValue attributes, DynU8 *sb,
                                      Arena *arena) {
  for (u64 i = 0; i < attributes.len; i++) {
    KeyValue attr = dyn_at(attributes, i);
    ASSERT(-1 == string_indexof_string(attr.key, S("\"")));

    *dyn_push(sb, arena) = ' ';
    dyn_append_slice(sb, attr.key, arena);
    *dyn_push(sb, arena) = '=';
    *dyn_push(sb, arena) = '"';
    // TODO: escape string.
    dyn_append_slice(sb, attr.value, arena);
    *dyn_push(sb, arena) = '"';
  }
}

static void html_tags_to_string(DynHtmlElements elements, DynU8 *sb,
                                Arena *arena);
static void html_tag_to_string(HtmlElement e, DynU8 *sb, Arena *arena);

static void html_tags_to_string(DynHtmlElements elements, DynU8 *sb,
                                Arena *arena) {
  for (u64 i = 0; i < elements.len; i++) {
    HtmlElement e = dyn_at(elements, i);
    html_tag_to_string(e, sb, arena);
  }
}

[[maybe_unused]]
static void html_document_to_string(HtmlDocument doc, DynU8 *sb, Arena *arena) {
  dyn_append_slice(sb, S("<!DOCTYPE html>"), arena);

  dyn_append_slice(sb, S("<html>"), arena);
  html_tag_to_string(doc.head, sb, arena);
  html_tag_to_string(doc.body, sb, arena);
  dyn_append_slice(sb, S("</html>"), arena);
}

static void html_tag_to_string(HtmlElement e, DynU8 *sb, Arena *arena) {
  static const String tag_to_string[HTML_MAX] = {
      [HTML_NONE] = S("FIXME"),
      [HTML_TITLE] = S("title"),
      [HTML_SPAN] = S("span"),
      [HTML_INPUT] = S("input"),
      [HTML_BUTTON] = S("button"),
      [HTML_LINK] = S("link"),
      [HTML_META] = S("meta"),
      [HTML_HEAD] = S("head"),
      [HTML_BODY] = S("body"),
      [HTML_DIV] = S("div"),
      [HTML_TEXT] = S("span"),
      [HTML_FORM] = S("form"),
      [HTML_FIELDSET] = S("fieldset"),
      [HTML_LABEL] = S("label"),
      [HTML_SCRIPT] = S("script"),
      [HTML_STYLE] = S("style"),
      [HTML_LEGEND] = S("legend"),
      [HTML_OL] = S("ol"),
      [HTML_LI] = S("li"),
  };

  ASSERT(!(HTML_NONE == e.kind || HTML_MAX == e.kind));

  *dyn_push(sb, arena) = '<';
  dyn_append_slice(sb, tag_to_string[e.kind], arena);
  html_attributes_to_string(e.attributes, sb, arena);
  *dyn_push(sb, arena) = '>';

  switch (e.kind) {
  // Cases of tag without any children and no closing tag.
  case HTML_LINK:
    [[fallthrough]];
  case HTML_META:
    ASSERT(0 == e.children.len);
    return;

  // 'Normal' tags.
  case HTML_OL:
    [[fallthrough]];
  case HTML_LI:
    [[fallthrough]];
  case HTML_HEAD:
    [[fallthrough]];
  case HTML_DIV:
    [[fallthrough]];
  case HTML_FORM:
    [[fallthrough]];
  case HTML_FIELDSET:
    [[fallthrough]];
  case HTML_LABEL:
    [[fallthrough]];
  case HTML_INPUT:
    [[fallthrough]];
  case HTML_SPAN:
    [[fallthrough]];
  case HTML_BODY:
    html_tags_to_string(e.children, sb, arena);
    break;

  // Only cases where `.text` is valid.
  case HTML_BUTTON:
    [[fallthrough]];
  case HTML_SCRIPT:
    [[fallthrough]];
  case HTML_STYLE:
    [[fallthrough]];
  case HTML_LEGEND:
    [[fallthrough]];
  case HTML_TITLE:
    [[fallthrough]];
  case HTML_TEXT:
    dyn_append_slice(sb, e.text, arena);
    break;

  // Invalid cases.
  case HTML_NONE:
    [[fallthrough]];
  case HTML_MAX:
    [[fallthrough]];
  default:
    ASSERT(0);
  }

  dyn_append_slice(sb, S("</"), arena);
  dyn_append_slice(sb, tag_to_string[e.kind], arena);
  *dyn_push(sb, arena) = '>';
}

[[maybe_unused]] [[nodiscard]] static String
http_req_extract_cookie_with_name(HttpRequest req, String cookie_name,
                                  Arena *arena) {
  String res = {0};
  {
    for (u64 i = 0; i < req.headers.len; i++) {
      KeyValue h = slice_at(req.headers, i);

      if (!string_ieq_ascii(h.key, S("Cookie"), arena)) {
        continue;
      }
      if (slice_is_empty(h.value)) {
        continue;
      }

      SplitIterator it_semicolon = string_split(h.value, ';');
      for (u64 j = 0; j < h.value.len; j++) {
        SplitResult split_semicolon = string_split_next(&it_semicolon);
        if (!split_semicolon.ok) {
          break;
        }

        SplitIterator it_equals = string_split(split_semicolon.s, '=');
        SplitResult split_equals_left = string_split_next(&it_equals);
        if (!split_equals_left.ok) {
          break;
        }
        if (!string_eq(split_equals_left.s, cookie_name)) {
          // Could be: `; Secure;`
          continue;
        }
        SplitResult split_equals_right = string_split_next(&it_equals);
        if (!slice_is_empty(split_equals_right.s)) {
          return split_equals_right.s;
        }
      }
    }
  }
  return res;
}

// NOTE: Only sanitation for including the string inside an HTML tag e.g.:
// `<div>...ESCAPED_STRING..</div>`.
// To include the string inside other context (e.g. JS, CSS, HTML attributes,
// etc), more advance sanitation is required.
[[maybe_unused]] [[nodiscard]] static String html_sanitize(String s,
                                                           Arena *arena) {
  DynU8 res = {0};
  dyn_ensure_cap(&res, s.len, arena);
  for (u64 i = 0; i < s.len; i++) {
    u8 c = slice_at(s, i);

    if ('&' == c) {
      dyn_append_slice(&res, S("&amp"), arena);
    } else if ('<' == c) {
      dyn_append_slice(&res, S("&lt"), arena);
    } else if ('>' == c) {
      dyn_append_slice(&res, S("&gt"), arena);
    } else if ('"' == c) {
      dyn_append_slice(&res, S("&quot"), arena);
    } else if ('\'' == c) {
      dyn_append_slice(&res, S("&#x27"), arena);
    } else {
      *dyn_push(&res, arena) = c;
    }
  }

  return dyn_slice(String, res);
}

typedef struct {
  String scheme;
  String username, password;
  String host; // Including subdomains.
  DynString path_components;
  // TODO: DynKeyValue url_parameters;
  u16 port;
  // TODO: fragment.
} Url;

typedef struct {
  bool ok;
  Url url;
} ParseUrlResult;

[[maybe_unused]] [[nodiscard]] static ParseUrlResult url_parse(String s,
                                                               Arena *arena) {
  ParseUrlResult res = {0};

  String remaining = s;

  // Scheme, mandatory.
  {
    String scheme_sep = S("://");
    i64 scheme_sep_idx = string_indexof_string(remaining, scheme_sep);
    if (scheme_sep_idx <= 1) { // Absent/empty scheme.
      return res;
    }
    res.url.scheme = slice_range(remaining, 0, (u64)scheme_sep_idx);
    remaining = slice_range(remaining, (u64)scheme_sep_idx + scheme_sep.len, 0);
  }

  // Username/password, optional.
  {
    i64 user_password_sep_idx = string_indexof_byte(remaining, '@');
    if (user_password_sep_idx >= 0) {
      ASSERT(0 && "TODO");
    }
  }

  // Host, mandatory (?).
  {
    i64 any_sep_idx = string_indexof_any_byte(remaining, S(":/?#"));
    if (-1 == any_sep_idx) {
      res.url.host = remaining;
      if (0 == res.url.host.len) {
        return res;
      }

      res.ok = true;
      return res;
    }

    res.url.host = slice_range(remaining, 0, (u64)any_sep_idx);
    if (0 == res.url.host.len) {
      return res;
    }

    bool is_port_sep = slice_at(remaining, any_sep_idx) == ':';
    remaining =
        slice_range(remaining, (u64)any_sep_idx + (is_port_sep ? 1 : 0), 0);

    if (is_port_sep) {
      ParseNumberResult port_parse = string_parse_u64(remaining);
      if (!port_parse.present) { // Empty/invalid port.
        return res;
      }
      if (port_parse.n > UINT16_MAX) { // Port too big.
        return res;
      }
      if (0 == port_parse.n) { // Zero port e.g. `http://abc:0`.
        return res;
      }
      res.url.port = (u16)port_parse.n;
      remaining = port_parse.remaining;
    }
  }

  // Path, optional.
  // Query parameters, optional.
  // FIXME: Messy code.
  {
    i64 any_sep_idx = string_indexof_any_byte(remaining, S("/?#"));
    if (-1 == any_sep_idx) {
      res.ok = true;
      return res;
    }

    bool is_sep_path = slice_at(remaining, any_sep_idx) == '/';
    bool is_sep_fragment = slice_at(remaining, any_sep_idx) == '#';
    bool is_sep_query_params = slice_at(remaining, any_sep_idx) == '?';
    if (is_sep_query_params) {
      ASSERT(0 && "TODO");
    }
    if (is_sep_fragment) {
      ASSERT(0 && "TODO");
    } else if (is_sep_path) {
      if (any_sep_idx != 0) {
        return res;
      }

      String path_raw = slice_range(remaining, (u64)any_sep_idx + 1, 0);
      res.url.path_components =
          http_parse_relative_path(path_raw, false, arena);
    } else {
      ASSERT(0);
    }
  }

  res.ok = true;
  return res;
}
#endif
