#pragma once

#include "lib.c"
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint64_t HTTP_REQUEST_LINES_MAX_COUNT = 512;
static const uint64_t HTTP_SERVER_HANDLER_MEM_LEN = 8192;
static const uint16_t HTTP_SERVER_DEFAULT_PORT = 12345;
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
  uint64_t len, cap;
} DynKeyValue;

typedef struct {
  String id;
  String path_raw;
  DynString path_components;
  HttpMethod method;
  DynKeyValue headers;
  String body;
  Error err;
} HttpRequest;

typedef struct {
  uint16_t status;
  DynKeyValue headers;
  Error err;

  // TODO: union{file_path,body}?
  String file_path;
  String body;
} HttpResponse;

typedef struct {
  String slice;
  Error err;
} IoOperationResult;

typedef IoOperationResult (*ReadFn)(void *ctx, void *buf, size_t buf_len);

typedef struct {
  uint64_t buf_idx;
  DynU8 buf;
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

[[nodiscard]] static Error writer_write_all(Writer writer, String slice) {
  for (uint64_t idx = 0; idx < slice.len;) {
    const String to_write = slice_range(slice, idx, 0);
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

  String slice = {.data = tmp, .len = res.slice.len};

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
reader_read_until_slice(Reader *reader, String needle, Arena *arena) {
  ASSERT(reader->buf.len >= reader->buf_idx);

  IoOperationResult io = {0};

  for (uint64_t i = 0; i < 128; i++) // FIXME
  {
    io = reader_read(reader, arena);
    if (io.err) {
      return io;
    }

    int64_t idx = string_indexof_string(io.slice, needle);
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
  const String NEWLINE = S("\r\n");

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

[[nodiscard]] static DynString http_parse_relative_path(String s,
                                                        Arena *arena) {
  ASSERT(string_starts_with(s, S("/")));

  DynString res = {0};

  SplitIterator split_it_question = string_split(s, '?');
  String work = string_split_next(&split_it_question).slice;

  SplitIterator split_it_slash = string_split(work, '/');
  for (uint64_t i = 0; i < s.len; i++) { // Bound.
    SplitResult split = string_split_next(&split_it_slash);
    if (!split.ok) {
      break;
    }

    if (slice_is_empty(split.slice)) {
      continue;
    }

    *dyn_push(&res, arena) = split.slice;
  }

  return res;
}

[[nodiscard]] static String make_unique_id_u128_string(Arena *arena) {
  __uint128_t id = 0;
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

    if (string_eq(method.slice, S("GET"))) {
      req.method = HM_GET;
    } else if (string_eq(method.slice, S("POST"))) {
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

    if (slice_is_empty(path.slice)) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (path.slice.data[0] != '/') {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    req.path_raw = path.slice;
    req.path_components = http_parse_relative_path(path.slice, arena);
  }

  {
    SplitResult http_version = string_split_next(&it);
    if (!http_version.ok) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }

    if (!string_eq(http_version.slice, S("HTTP/1.1"))) {
      req.err = HS_ERR_INVALID_HTTP_REQUEST;
      return req;
    }
  }

  return req;
}

[[nodiscard]] static Error
reader_read_headers(Reader *reader, DynKeyValue *headers, Arena *arena) {
  dyn_ensure_cap(headers, 30, arena);

  for (uint64_t _i = 0; _i < HTTP_REQUEST_LINES_MAX_COUNT; _i++) {
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

    String key_trimmed = string_trim(key.slice, ' ');

    String value = it.slice; // Remainder.
    String value_trimmed = string_trim(value, ' ');

    KeyValue header = {.key = key_trimmed, .value = value_trimmed};
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
request_parse_content_length_maybe(HttpRequest req, Arena *arena) {
  ASSERT(!req.err);

  for (uint64_t i = 0; i < req.headers.len; i++) {
    KeyValue h = req.headers.data[i];

    if (!string_ieq_ascii(S("Content-Length"), h.key, arena)) {
      continue;
    }

    return string_parse_u64_decimal(h.value);
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
  ASSERT(slice_is_empty(res.file_path) || slice_is_empty(res.body));

  DynU8 sb = {0};

  dyn_append_slice(&sb, S("HTTP/1.1 "), arena);
  dynu8_append_u64(&sb, res.status, arena);
  dyn_append_slice(&sb, S("\r\n"), arena);

  for (uint64_t i = 0; i < res.headers.len; i++) {
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

  const String slice = dyn_slice(String, sb);

  Error err = writer_write_all(writer, slice);
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

    err = os_sendfile(file_fd, (int)(uint64_t)writer.ctx, (uint64_t)st.st_size);
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

static void http_response_register_file_for_sending(HttpResponse *res,
                                                    String path) {
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

  const uint64_t mem_use = HTTP_SERVER_HANDLER_MEM_LEN -
                           ((uint64_t)arena.end - (uint64_t)arena.start);
  log(LOG_LEVEL_INFO, "http request end", &arena, L("arena_use", mem_use),
      L("req.path", req.path_raw), L("req.headers.len", req.headers.len),
      L("res.headers.len", res.headers.len), L("status", res.status),
      L("req.method", http_method_to_s(req.method)),
      L("res.file_path", res.file_path), L("res.body.len", res.body.len),
      L("req.id", req.id));

  close(socket);
}

static Error http_server_run(uint16_t port, HttpRequestHandleFn request_handler,
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
          L("arena.available", (uint64_t)arena->end - (uint64_t)arena->start));
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

[[maybe_unused]] [[nodiscard]] static HttpResponse
http_client_request(struct sockaddr *addr, uint32_t addr_sizeof,
                    HttpRequest req, Arena *arena) {
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

  int client = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == client) {
    res.err = (Error)errno;
    return res;
  }

  if (-1 == connect(client, addr, addr_sizeof)) {
    res.err = (Error)errno;
    goto end;
  }

  DynU8 sb = {0};
  dyn_append_slice(&sb, http_method_to_s(req.method), arena);
  dyn_append_slice(&sb, S(" /"), arena);

  for (uint64_t i = 0; i < req.path_components.len; i++) {
    String path_component = dyn_at(req.path_components, i);
    // TODO: Need to url encode?
    dyn_append_slice(&sb, path_component, arena);
    *dyn_push(&sb, arena) = '/';
  }
  dyn_append_slice(&sb, S(" HTTP/1.1"), arena);
  dyn_append_slice(&sb, S("\r\n"), arena);

  for (uint64_t i = 0; i < req.headers.len; i++) {
    KeyValue header = dyn_at(req.headers, i);
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

    String http_version_needle = S("HTTP/1.1 ");
    if (!string_starts_with(status_line.line, http_version_needle)) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    String status_str =
        slice_range(status_line.line, http_version_needle.len, 0);
    ParseNumberResult status_parsed = string_parse_u64_decimal(status_str);
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

typedef struct {
  String key, value;
} FormDataKV;

typedef struct {
  FormDataKV *data;
  uint64_t len, cap;
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
form_data_kv_parse_element(String in, uint8_t ch_terminator, Arena *arena) {
  FormDataKVElementParseResult res = {0};
  DynU8 data = {0};

  uint64_t i = 0;
  for (; i < in.len; i++) {
    uint8_t c = in.data[i];

    if ('+' == c) {
      *dyn_push(&data, arena) = ' ';
    } else if ('%' == c) {
      if ((in.len - i) < 2) {
        res.err = HS_ERR_INVALID_FORM_DATA;
        return res;
      }
      uint8_t c1 = in.data[i + 1];
      uint8_t c2 = in.data[i + 2];

      if (!(ch_is_hex_digit(c1) && ch_is_hex_digit(c2))) {
        res.err = HS_ERR_INVALID_FORM_DATA;
        return res;
      }

      uint8_t utf8_character = ch_from_hex(c1) * 16 + ch_from_hex(c2);
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

[[nodiscard]] static FormDataParseResult form_data_parse(String in,
                                                         Arena *arena) {
  FormDataParseResult res = {0};

  String remaining = in;

  for (uint64_t i = 0; i < in.len; i++) { // Bound.
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
  HTML_LINK,
  HTML_META,
  HTML_HEAD,
  HTML_BODY,
  HTML_DIV,
  HTML_TEXT,
  HTML_FORM,
  HTML_FIELDSET,
  HTML_LABEL,
  HTML_SCRIPT,
  HTML_STYLE,
} HtmlKind;

typedef struct HtmlElement HtmlElement;
typedef struct {
  HtmlElement *data;
  uint64_t len, cap;
} DynHtmlElements;

struct HtmlElement {
  HtmlKind kind;
  DynKeyValue attributes;
  union {
    DynHtmlElements children;
    String text;
  };
};

typedef struct {
  HtmlElement body;
  HtmlElement head;
} HtmlDocument;

[[nodiscard]] static HtmlDocument html_make(String title, Arena *arena) {
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
  for (uint64_t i = 0; i < attributes.len; i++) {
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
  for (uint64_t i = 0; i < elements.len; i++) {
    HtmlElement e = dyn_at(elements, i);
    html_tag_to_string(e, sb, arena);
  }
}

static void html_document_to_string(HtmlDocument doc, DynU8 *sb, Arena *arena) {
  dyn_append_slice(sb, S("<!DOCTYPE html>"), arena);

  dyn_append_slice(sb, S("<html>"), arena);
  html_tag_to_string(doc.head, sb, arena);
  html_tag_to_string(doc.body, sb, arena);
  dyn_append_slice(sb, S("</html>"), arena);
}

static void html_tag_to_string(HtmlElement e, DynU8 *sb, Arena *arena) {
  switch (e.kind) {
  case HTML_NONE:
    ASSERT(0);
  case HTML_TITLE:
    ASSERT(0 == e.attributes.len);
    dyn_append_slice(sb, S("<title>"), arena);
    dyn_append_slice(sb, e.text, arena);
    dyn_append_slice(sb, S("</title>"), arena);
    break;
  case HTML_LINK:
    dyn_append_slice(sb, S("<link"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    dyn_append_slice(sb, S(">"), arena);
    break;
  case HTML_META:
    ASSERT(0 == e.children.len);
    dyn_append_slice(sb, S("<meta"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    break;
  case HTML_HEAD:
    ASSERT(0 == e.attributes.len);
    dyn_append_slice(sb, S("<head>"), arena);
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</head>"), arena);
    break;
  case HTML_SCRIPT:
    ASSERT(0 == e.attributes.len);
    dyn_append_slice(sb, S("<script>"), arena);

    ASSERT(0 == e.children.len || 1 == e.children.len);
    if (1 == e.children.len) {
      ASSERT(HTML_TEXT == dyn_at(e.children, 0).kind);
    }
    html_tags_to_string(e.children, sb, arena);

    dyn_append_slice(sb, S("</script>"), arena);
    break;
  case HTML_STYLE:
    ASSERT(0 == e.attributes.len);
    dyn_append_slice(sb, S("<style>"), arena);

    ASSERT(0 == e.children.len || 1 == e.children.len);
    if (1 == e.children.len) {
      ASSERT(HTML_TEXT == dyn_at(e.children, 0).kind);
    }
    html_tags_to_string(e.children, sb, arena);

    dyn_append_slice(sb, S("</style>"), arena);
    break;
  case HTML_BODY:
    dyn_append_slice(sb, S("<body"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</body>"), arena);
    break;
  case HTML_DIV:
    dyn_append_slice(sb, S("<div"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</div>"), arena);
    break;
  case HTML_FORM:
    dyn_append_slice(sb, S("<form"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</form>"), arena);
    break;
  case HTML_FIELDSET:
    dyn_append_slice(sb, S("<fieldset"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</fieldset>"), arena);
    break;
  case HTML_LABEL:
    dyn_append_slice(sb, S("<label"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</label>"), arena);
    break;
  case HTML_INPUT:
    dyn_append_slice(sb, S("<input"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</input>"), arena);
    break;
  case HTML_SPAN:
    dyn_append_slice(sb, S("<span"), arena);
    html_attributes_to_string(e.attributes, sb, arena);
    *dyn_push(sb, arena) = '>';
    html_tags_to_string(e.children, sb, arena);
    dyn_append_slice(sb, S("</span>"), arena);
    break;
  case HTML_TEXT:
    ASSERT(0 == e.attributes.len);
    dyn_append_slice(sb, e.text, arena);
    break;
  default:
    ASSERT(0);
  }
}
