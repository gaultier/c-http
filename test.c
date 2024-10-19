#include "http.c"
#include "lib.c"
#include <sys/wait.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static void test_slice_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == slice_indexof_slice((Slice){0}, S("fox"))); }

  // Empty needle.
  { ASSERT(-1 == slice_indexof_slice(S("hello"), (Slice){0})); }

  // Not found.
  { ASSERT(-1 == slice_indexof_slice(S("hello world"), S("foobar"))); }

  // Found, one occurence.
  { ASSERT(6 == slice_indexof_slice(S("hello world"), S("world"))); }

  // Found, first occurence.
  { ASSERT(6 == slice_indexof_slice(S("world hello hell"), S("hell"))); }

  // Found, second occurence.
  { ASSERT(10 == slice_indexof_slice(S("hello fox foxy"), S("foxy"))); }

  // Almost found, prefix matches.
  { ASSERT(-1 == slice_indexof_slice(S("hello world"), S("worldly"))); }
}

typedef struct {
  Slice slice;
  uint64_t idx;
} MemReadContext;

static IoOperationResult reader_read_from_slice(void *ctx, void *buf,
                                                size_t buf_len) {
  MemReadContext *mem_ctx = ctx;

  ASSERT(buf != nullptr);
  ASSERT(mem_ctx->slice.data != nullptr);
  if (mem_ctx->idx >= mem_ctx->slice.len) {
    // End.
    return (IoOperationResult){0};
  }

  const uint64_t remaining = mem_ctx->slice.len - mem_ctx->idx;
  const uint64_t can_fill = MIN(remaining, buf_len);
  ASSERT(can_fill <= remaining);

  IoOperationResult res = {
      .slice.data = mem_ctx->slice.data + mem_ctx->idx,
      .slice.len = can_fill,
  };
  memcpy(buf, res.slice.data, res.slice.len);

  mem_ctx->idx += can_fill;
  ASSERT(mem_ctx->idx <= mem_ctx->slice.len);
  return res;
}

static Reader reader_make_from_slice(MemReadContext *ctx) {
  return (Reader){
      .ctx = ctx,
      .read_fn = reader_read_from_slice,
  };
}

static void test_read_http_request_without_body() {
  Slice req_slice = S("GET /foo?bar=2 HTTP/1.1\r\nHost: "
                      "localhost:12345\r\nAccept: */*\r\n\r\n");
  MemReadContext ctx = {.slice = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4096);
  const HttpRequest req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_GET == req.method);
  ASSERT(slice_eq(req.path, S("/foo?bar=2")));

  ASSERT(2 == req.headers.len);
  ASSERT(slice_eq(dyn_at(req.headers, 0).value, S("localhost:12345")));
  ASSERT(slice_eq(dyn_at(req.headers, 1).key, S("Accept")));
  ASSERT(slice_eq(dyn_at(req.headers, 1).value, S("*/*")));
}

static void test_read_http_request_with_body() {
  Slice req_slice = S("POST /foo?bar=2 HTTP/1.1\r\nContent-Length: 13\r\nHost: "
                      "localhost:12345\r\nAccept: */*\r\n\r\nhello\r\nworld!");
  MemReadContext ctx = {.slice = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4096);
  const HttpRequest req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_POST == req.method);
  ASSERT(slice_eq(req.path, S("/foo?bar=2")));

  ASSERT(3 == req.headers.len);
  ASSERT(slice_eq(dyn_at(req.headers, 0).key, S("Content-Length")));
  ASSERT(slice_eq(dyn_at(req.headers, 0).value, S("13")));
  ASSERT(slice_eq(dyn_at(req.headers, 1).key, S("Host")));
  ASSERT(slice_eq(dyn_at(req.headers, 1).value, S("localhost:12345")));
  ASSERT(slice_eq(dyn_at(req.headers, 2).key, S("Accept")));
  ASSERT(slice_eq(dyn_at(req.headers, 2).value, S("*/*")));

  ASSERT(slice_eq(req.body, S("hello\r\nworld!")));
}

static void test_slice_trim() {
  Slice trimmed = slice_trim(S("   foo "), ' ');
  ASSERT(slice_eq(trimmed, S("foo")));
}

static void test_slice_split() {
  Slice slice = S("hello..world...foobar");
  SplitIterator it = slice_split_it(slice, '.');

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("hello")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("world")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("foobar")));
  }

  ASSERT(false == slice_split_next(&it).ok);
  ASSERT(false == slice_split_next(&it).ok);
}

static void test_log_entry_quote_value() {
  Arena arena = arena_make_from_virtual_mem(4096);
  // Nothing to escape.
  {
    Slice s = S("hello");
    Slice expected = S("\"hello\"");
    ASSERT(slice_eq(expected, log_entry_quote_value(s, &arena)));
  }
  {
    Slice s = S("{\"id\": 1}");
    Slice expected = S("\"{\\\"id\\\": 1}\"");
    ASSERT(slice_eq(expected, log_entry_quote_value(s, &arena)));
  }
}

static void test_make_log_line() {
  Arena arena = arena_make_from_virtual_mem(4096);

  Slice log_line =
      make_log_line(LOG_LEVEL_DEBUG, S("foobar"), &arena, 2, LCI("num", 42),
                    LCS("slice", S("hello \"world\"")));

  Slice expected =
      S("message=\"foobar\" num=42 slice=\"hello \\\"world\\\"\"\n");
  ASSERT(slice_starts_with(log_line, S("level=debug timestamp_ns=")));
  ASSERT(slice_ends_with(log_line, expected));
}

static void test_dyn_ensure_cap() {
  uint64_t arena_cap = 4096;

  // Trigger the optimization when the last allocation in the arena gets
  // extended.
  {
    Arena arena = arena_make_from_virtual_mem(arena_cap);

    DynArrayU8 dyn = {0};
    *dyn_push(&dyn, &arena) = 1;
    ASSERT(1 == dyn.len);
    ASSERT(2 == dyn.cap);

    uint64_t arena_size_expected =
        arena_cap - ((uint64_t)arena.end - (uint64_t)arena.start);
    ASSERT(2 == arena_size_expected);
    ASSERT(dyn.cap == arena_size_expected);

    uint64_t desired_cap = 13;
    dyn_ensure_cap(&dyn, desired_cap, &arena);
    ASSERT(16 == dyn.cap);
    arena_size_expected =
        arena_cap - ((uint64_t)arena.end - (uint64_t)arena.start);
    ASSERT(16 == arena_size_expected);
  }
  // General case.
  {
    Arena arena = arena_make_from_virtual_mem(arena_cap);

    DynArrayU8 dyn = {0};
    *dyn_push(&dyn, &arena) = 1;
    ASSERT(1 == dyn.len);
    ASSERT(2 == dyn.cap);

    DynArrayU8 dummy = {0};
    *dyn_push(&dummy, &arena) = 2;
    *dyn_push(&dummy, &arena) = 3;

    uint64_t arena_size_expected =
        arena_cap - ((uint64_t)arena.end - (uint64_t)arena.start);
    ASSERT(2 + 2 == arena_size_expected);

    // This triggers a new allocation.
    *dyn_push(&dummy, &arena) = 4;
    ASSERT(3 == dummy.len);
    ASSERT(4 == dummy.cap);

    arena_size_expected =
        arena_cap - ((uint64_t)arena.end - (uint64_t)arena.start);
    ASSERT(2 + 4 == arena_size_expected);

    uint64_t desired_cap = 13;
    dyn_ensure_cap(&dyn, desired_cap, &arena);
    ASSERT(16 == dyn.cap);

    arena_size_expected =
        arena_cap - ((uint64_t)arena.end - (uint64_t)arena.start);
    ASSERT(16 + 6 == arena_size_expected);
  }
}

static HttpResponse handle_request_post(HttpRequest req, Arena *arena) {
  ASSERT(HM_POST == req.method);
  ASSERT(slice_eq(S("foo\nbar"), req.body));
  ASSERT(slice_eq(S("/comment"), req.path));

  HttpResponse res = {0};
  res.status = 201;
  res.body = S("hello world!");
  http_push_header(&res.headers, S("Connection"), S("close"), arena);
  http_push_header(&res.headers, S("Content-Type"), S("text/plain"), arena);

  return res;
}

static uint16_t random_port() {
  uint16_t max_port = UINT16_MAX;
  uint16_t min_port = 3000;

  uint16_t port = (uint16_t)arc4random_uniform(max_port);
  CLAMP(&port, min_port, max_port);

  return port;
}

static void test_http_server_post() {
  Arena arena = arena_make_from_virtual_mem(4096);

  // The http server runs in its own child process.
  // The parent process acts as a HTTP client contacting the server.
  uint16_t port = random_port();

  pid_t pid = fork();
  ASSERT(-1 != pid);
  if (pid == 0) { // Child
    ASSERT(0 == http_server_run(port, handle_request_post, &arena));

  } else { // Parent

    for (uint64_t i = 0; i < 5; i++) {
      struct sockaddr_in addr = {
          .sin_family = AF_INET,
          .sin_port = htons(port),
      };
      HttpRequest req = {
          .method = HM_POST,
          .path = S("/comment"),
          .body = S("foo\nbar"),
      };
      http_push_header(&req.headers, S("Content-Type"), S("text/plain"),
                       &arena);
      http_push_header(&req.headers, S("Content-Length"), S("7"), &arena);
      HttpResponse resp = http_client_request((struct sockaddr *)&addr,
                                              sizeof(addr), req, &arena);

      if (!resp.err) {
        ASSERT(201 == resp.status);
        ASSERT(slice_eq(S("hello world!"), resp.body));
        ASSERT(2 == resp.headers.len);

        HttpHeader h1 = dyn_at(resp.headers, 0);
        ASSERT(slice_eq(S("Connection"), h1.key));
        ASSERT(slice_eq(S("close"), h1.value));

        HttpHeader h2 = dyn_at(resp.headers, 1);
        ASSERT(slice_eq(S("Content-Type"), h2.key));
        ASSERT(slice_eq(S("text/plain"), h2.value));

        // Stop the http server and check it had no issue.
        {
          ASSERT(-1 != kill(pid, SIGKILL));
          int child_status = 0;
          ASSERT(-1 != waitpid(pid, &child_status, 0));
          ASSERT(true == WIFSIGNALED(child_status));
          ASSERT(0 == WEXITSTATUS(child_status));
        }
        return;
      }

      // Retry.
      usleep(10'000);
    }
    ASSERT(false);
  }
}

static HttpResponse handle_request_file(HttpRequest req, Arena *arena) {
  ASSERT(HM_GET == req.method);
  ASSERT(slice_is_empty(req.body));
  ASSERT(slice_eq(S("/index.html"), req.path));

  HttpResponse res = {0};
  res.status = 200;
  http_response_register_file_for_sending(&res, "index.html");
  http_push_header(&res.headers, S("Connection"), S("close"), arena);
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

static void test_http_server_serve_file() {
  Arena arena = arena_make_from_virtual_mem(4096);

  // The http server runs in its own child process.
  // The parent process acts as a HTTP client contacting the server.
  uint16_t port = random_port();

  pid_t pid = fork();
  ASSERT(-1 != pid);
  if (pid == 0) { // Child
    ASSERT(0 == http_server_run(port, handle_request_file, &arena));

  } else { // Parent

    for (uint64_t i = 0; i < 5; i++) {
      struct sockaddr_in addr = {
          .sin_family = AF_INET,
          .sin_port = htons(port),
      };
      HttpRequest req = {
          .method = HM_GET,
          .path = S("/index.html"),
      };
      HttpResponse resp = http_client_request((struct sockaddr *)&addr,
                                              sizeof(addr), req, &arena);

      if (!resp.err) {
        ASSERT(200 == resp.status);

        ASSERT(2 == resp.headers.len);

        HttpHeader h1 = dyn_at(resp.headers, 0);
        ASSERT(slice_eq(S("Connection"), h1.key));
        ASSERT(slice_eq(S("close"), h1.value));

        HttpHeader h2 = dyn_at(resp.headers, 1);
        ASSERT(slice_eq(S("Content-Type"), h2.key));
        ASSERT(slice_eq(S("text/html"), h2.value));

        int file = open("index.html", O_RDONLY);
        ASSERT(-1 != file);
        struct stat st = {0};
        ASSERT(-1 != stat("index.html", &st));

        ASSERT(st.st_size >= 0);
        ASSERT((uint64_t)st.st_size == resp.body.len);

        void *file_content = mmap(nullptr, (uint64_t)st.st_size, PROT_READ,
                                  MAP_PRIVATE, file, 0);
        ASSERT(nullptr != file_content);

        Slice file_content_slice = {.data = file_content,
                                    .len = (uint64_t)st.st_size};
        ASSERT(slice_eq(file_content_slice, resp.body));

        // Stop the http server and check it had no issue.
        {
          ASSERT(-1 != kill(pid, SIGKILL));
          int child_status = 0;
          ASSERT(-1 != waitpid(pid, &child_status, 0));
          ASSERT(true == WIFSIGNALED(child_status));
          ASSERT(0 == WEXITSTATUS(child_status));
        }
        return;
      }

      // Retry.
      usleep(10'000);
    }
    ASSERT(false);
  }
}

typedef void (*TestFn)();

int main() {
  test_slice_indexof_slice();
  test_slice_trim();
  test_slice_split();
  test_read_http_request_without_body();
  test_read_http_request_with_body();
  test_log_entry_quote_value();
  test_make_log_line();
  test_dyn_ensure_cap();
  test_http_server_post();
  test_http_server_serve_file();
}
