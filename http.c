#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <sys/sendfile.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lib.c"

static const uint64_t MAX_REQUEST_LINES = 512;
static const uint64_t CLIENT_MEM = 8192;
static const uint16_t PORT = 12345;
static const int LISTEN_BACKLOG = 1024;
#define MAX_LINE_LENGTH 1024

typedef enum {
  HS_ERR_INVALID_HTTP_REQUEST,
} HS_ERROR;

typedef enum { HM_UNKNOWN, HM_GET, HM_POST } HttpMethod;

typedef struct {
  Slice key, value;
} HttpHeader;

typedef struct {
  HttpHeader *data;
  uint64_t len, cap;
} DynArrayHttpHeaders;

typedef struct {
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

MUST_USE static IoOperationResult
line_buffered_reader_read_from_socket(void *ctx, void *buf, size_t buf_len) {
  const ssize_t n_read = recv((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_read == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.bytes_count = n_read};
}

MUST_USE static LineBufferedReader
line_buffered_reader_make_from_socket(int socket) {
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

MUST_USE static IoOperationResult
writer_write_from_socket(void *ctx, const void *buf, size_t buf_len) {
  const ssize_t n_written = send((int)(uint64_t)ctx, buf, buf_len, 0);
  if (n_written == -1) {
    return (IoOperationResult){.err = errno};
  }

  return (IoOperationResult){.bytes_count = n_written};
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

MUST_USE static LineRead
line_buffered_reader_consume(LineBufferedReader *reader) {
  const Slice NEWLINE = S("\r\n");

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

MUST_USE static LineRead line_buffered_reader_read(LineBufferedReader *reader,
                                                   Arena *arena) {
  LineRead line = {0};

  for (uint64_t _i = 0; _i < 10; _i++) {
    line = line_buffered_reader_consume(reader);
    if (line.present) {
      return line;
    }

    uint8_t buf[MAX_LINE_LENGTH] = {0};
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

MUST_USE static HttpRequest request_parse_status_line(LineRead status_line) {
  HttpRequest res = {0};

  if (!status_line.present) {
    res.err = HS_ERR_INVALID_HTTP_REQUEST;
    return res;
  }
  if (status_line.err) {
    res.err = status_line.err;
    return res;
  }

  SplitIterator it = slice_split_it(status_line.line, ' ');

  HttpRequest req = {0};

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
                                                 LineBufferedReader *reader,
                                                 Arena *arena) {
  if (req.err) {
    return req;
  }

  HttpRequest res = req;

  for (uint64_t _i = 0; _i < MAX_REQUEST_LINES; _i++) {
    const LineRead line = line_buffered_reader_read(reader, arena);

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

MUST_USE static HttpRequest request_read(LineBufferedReader *reader,
                                         Arena *arena) {
  const LineRead status_line = line_buffered_reader_read(reader, arena);
  HttpRequest req = request_parse_status_line(status_line);
  req = request_read_headers(req, reader, arena);

  return req;
}

MUST_USE static int response_write(Writer writer, HttpResponse res,
                                   Arena *arena) {
  // Invalid to both want to serve a file and a body.
  ASSERT(NULL == res.file_path || slice_is_empty(res.body));

  DynArrayU8 sb = {0};

  dyn_array_u8_append_cstr(&sb, "HTTP/1.1 ", arena);
  dyn_array_u8_append_u16(&sb, res.status, arena);
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

    ssize_t sent =
        sendfile((int)(uint64_t)writer.ctx, file_fd, NULL, st.st_size);
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
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);

  Arena arena = arena_make_from_virtual_mem(CLIENT_MEM);
  LineBufferedReader reader = line_buffered_reader_make_from_socket(socket);
  const HttpRequest req = request_read(&reader, &arena);
  if (req.err) {
    exit(EINVAL);
  }

  HttpResponse res = handle(req, &arena);

  Writer writer = writer_make_from_socket(socket);
  (void)response_write(writer, res, &arena);

  struct timespec ts_end = {0};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
  fprintf(stderr, "[D001] arena_use=%ld duration=%lds%ldus\n",
          CLIENT_MEM - (arena.end - arena.start),
          ts_end.tv_sec - ts_start.tv_sec,
          (ts_end.tv_nsec - ts_start.tv_nsec) / 1000);
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