#include "http.c"
#include "lib.c"
#include <sys/wait.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static void test_string_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == string_indexof_string((String){0}, S("fox"))); }

  // Empty needle.
  { ASSERT(-1 == string_indexof_string(S("hello"), (String){0})); }

  // Not found.
  { ASSERT(-1 == string_indexof_string(S("hello world"), S("foobar"))); }

  // Found, one occurence.
  { ASSERT(6 == string_indexof_string(S("hello world"), S("world"))); }

  // Found, first occurence.
  { ASSERT(6 == string_indexof_string(S("world hello hell"), S("hell"))); }

  // Found, second occurence.
  { ASSERT(10 == string_indexof_string(S("hello fox foxy"), S("foxy"))); }

  // Almost found, prefix matches.
  { ASSERT(-1 == string_indexof_string(S("hello world"), S("worldly"))); }
}

typedef struct {
  String slice;
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
  String req_slice = S("GET /foo?bar=2 HTTP/1.1\r\nHost: "
                       "localhost:12345\r\nAccept: */*\r\n\r\n");
  MemReadContext ctx = {.slice = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4096);
  const HttpRequest req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_GET == req.method);
  ASSERT(string_eq(req.path_raw, S("/foo?bar=2")));

  ASSERT(2 == req.headers.len);
  ASSERT(string_eq(dyn_at(req.headers, 0).value, S("localhost:12345")));
  ASSERT(string_eq(dyn_at(req.headers, 1).key, S("Accept")));
  ASSERT(string_eq(dyn_at(req.headers, 1).value, S("*/*")));
}

static void test_read_http_request_with_body() {
  String req_slice =
      S("POST /foo?bar=2 HTTP/1.1\r\nContent-Length: 13\r\nHost: "
        "localhost:12345\r\nAccept: */*\r\n\r\nhello\r\nworld!");
  MemReadContext ctx = {.slice = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4096);
  const HttpRequest req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_POST == req.method);
  ASSERT(string_eq(req.path_raw, S("/foo?bar=2")));

  ASSERT(3 == req.headers.len);
  ASSERT(string_eq(dyn_at(req.headers, 0).key, S("Content-Length")));
  ASSERT(string_eq(dyn_at(req.headers, 0).value, S("13")));
  ASSERT(string_eq(dyn_at(req.headers, 1).key, S("Host")));
  ASSERT(string_eq(dyn_at(req.headers, 1).value, S("localhost:12345")));
  ASSERT(string_eq(dyn_at(req.headers, 2).key, S("Accept")));
  ASSERT(string_eq(dyn_at(req.headers, 2).value, S("*/*")));

  ASSERT(string_eq(req.body, S("hello\r\nworld!")));
}

static void test_string_trim() {
  String trimmed = string_trim(S("   foo "), ' ');
  ASSERT(string_eq(trimmed, S("foo")));
}

static void test_string_split() {
  String slice = S("hello..world...foobar");
  SplitIterator it = string_split(slice, '.');

  {
    SplitResult elem = string_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(string_eq(elem.slice, S("hello")));
  }

  {
    SplitResult elem = string_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(string_eq(elem.slice, S("world")));
  }

  {
    SplitResult elem = string_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(string_eq(elem.slice, S("foobar")));
  }

  ASSERT(false == string_split_next(&it).ok);
  ASSERT(false == string_split_next(&it).ok);
}

static void test_log_entry_quote_value() {
  Arena arena = arena_make_from_virtual_mem(4096);
  // Nothing to escape.
  {
    String s = S("hello");
    String expected = S("\"hello\"");
    ASSERT(string_eq(expected, json_escape_string(s, &arena)));
  }
  {
    String s = S("{\"id\": 1}");
    String expected = S("\"{\\\"id\\\": 1}\"");
    ASSERT(string_eq(expected, json_escape_string(s, &arena)));
  }
  {
    uint8_t backslash = 0x5c;
    uint8_t double_quote = '"';
    uint8_t data[] = {backslash, double_quote};
    String s = {.data = data, .len = sizeof(data)};

    uint8_t data_expected[] = {double_quote, backslash,    backslash,
                               backslash,    double_quote, double_quote};
    String expected = {.data = data_expected, .len = sizeof(data_expected)};
    ASSERT(string_eq(expected, json_escape_string(s, &arena)));
  }
}

static void test_make_log_line() {
  Arena arena = arena_make_from_virtual_mem(4096);

  String log_line =
      make_log_line(LOG_LEVEL_DEBUG, S("foobar"), &arena, 2, L("num", 42),
                    L("slice", S("hello \"world\"")));

  String expected_suffix = S(
      "\"message\":\"foobar\",\"num\":42,\"slice\":\"hello \\\"world\\\"\"}\n");
  ASSERT(string_starts_with(log_line,
                            S("{\"level\":\"debug\",\"monotonic_ns\":")));
  ASSERT(string_ends_with(log_line, expected_suffix));
}

static void test_dyn_ensure_cap() {
  uint64_t arena_cap = 4096;

  // Trigger the optimization when the last allocation in the arena gets
  // extended.
  {
    Arena arena = arena_make_from_virtual_mem(arena_cap);

    DynU8 dyn = {0};
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

    DynU8 dyn = {0};
    *dyn_push(&dyn, &arena) = 1;
    ASSERT(1 == dyn.len);
    ASSERT(2 == dyn.cap);

    DynU8 dummy = {0};
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

static HttpResponse handle_request_post(HttpRequest req, void *ctx,
                                        Arena *arena) {
  (void)ctx;

  ASSERT(HM_POST == req.method);
  ASSERT(string_eq(S("foo\nbar"), req.body));
  ASSERT(string_eq(S("/comment"), req.path_raw) ||
         string_eq(S("/comment/"), req.path_raw));
  ASSERT(1 == req.path_components.len);
  String path0 = dyn_at(req.path_components, 0);
  ASSERT(string_eq(path0, S("comment")));

  HttpResponse res = {0};
  res.status = 201;
  res.body = S("hello world!");
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
    ASSERT(0 == http_server_run(port, handle_request_post, nullptr, &arena));

  } else { // Parent

    for (uint64_t i = 0; i < 5; i++) {
      struct sockaddr_in addr = {
          .sin_family = AF_INET,
          .sin_port = htons(port),
      };
      HttpRequest req = {
          .method = HM_POST,
          .body = S("foo\nbar"),
      };
      *dyn_push(&req.path_components, &arena) = S("comment");

      http_push_header(&req.headers, S("Content-Type"), S("text/plain"),
                       &arena);
      http_push_header(&req.headers, S("Content-Length"), S("7"), &arena);
      HttpResponse resp = http_client_request((struct sockaddr *)&addr,
                                              sizeof(addr), req, &arena);

      if (!resp.err) {
        ASSERT(201 == resp.status);
        ASSERT(string_eq(S("hello world!"), resp.body));
        ASSERT(2 == resp.headers.len);

        HttpHeader h1 = dyn_at(resp.headers, 0);
        ASSERT(string_eq(S("Content-Type"), h1.key));
        ASSERT(string_eq(S("text/plain"), h1.value));

        HttpHeader h2 = dyn_at(resp.headers, 1);
        ASSERT(string_eq(S("Connection"), h2.key));
        ASSERT(string_eq(S("close"), h2.value));

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

static HttpResponse handle_request_file(HttpRequest req, void *ctx,
                                        Arena *arena) {
  (void)ctx;

  ASSERT(HM_GET == req.method);
  ASSERT(slice_is_empty(req.body));
  ASSERT(string_eq(S("/index.html"), req.path_raw) ||
         string_eq(S("/index.html/"), req.path_raw));
  ASSERT(1 == req.path_components.len);
  String path0 = dyn_at(req.path_components, 0);
  ASSERT(string_eq(path0, S("index.html")));

  HttpResponse res = {0};
  res.status = 200;
  http_response_register_file_for_sending(&res, S("index.html"));
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

static void test_http_server_serve_file() {
  Arena arena = arena_make_from_virtual_mem(8192);

  // The http server runs in its own child process.
  // The parent process acts as a HTTP client contacting the server.
  uint16_t port = random_port();

  pid_t pid = fork();
  ASSERT(-1 != pid);
  if (pid == 0) { // Child
    ASSERT(0 == http_server_run(port, handle_request_file, nullptr, &arena));

  } else { // Parent

    for (uint64_t i = 0; i < 5; i++) {
      struct sockaddr_in addr = {
          .sin_family = AF_INET,
          .sin_port = htons(port),
      };
      HttpRequest req = {
          .method = HM_GET,
      };
      *dyn_push(&req.path_components, &arena) = S("index.html");
      HttpResponse resp = http_client_request((struct sockaddr *)&addr,
                                              sizeof(addr), req, &arena);

      if (!resp.err) {
        ASSERT(200 == resp.status);

        ASSERT(2 == resp.headers.len);

        HttpHeader h1 = dyn_at(resp.headers, 0);
        ASSERT(string_eq(S("Content-Type"), h1.key));
        ASSERT(string_eq(S("text/html"), h1.value));

        HttpHeader h2 = dyn_at(resp.headers, 1);
        ASSERT(string_eq(S("Connection"), h2.key));
        ASSERT(string_eq(S("close"), h2.value));

        int file = open("index.html", O_RDONLY);
        ASSERT(-1 != file);
        struct stat st = {0};
        ASSERT(-1 != stat("index.html", &st));

        ASSERT(st.st_size >= 0);
        ASSERT((uint64_t)st.st_size == resp.body.len);

        void *file_content = mmap(nullptr, (uint64_t)st.st_size, PROT_READ,
                                  MAP_PRIVATE, file, 0);
        ASSERT(nullptr != file_content);

        String file_content_slice = {.data = file_content,
                                     .len = (uint64_t)st.st_size};
        ASSERT(string_eq(file_content_slice, resp.body));

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

static void test_form_data_parse() {
  Arena arena = arena_make_from_virtual_mem(4096);

  String form_data_raw =
      S("foo=bar&name=hello+world&option=%E6%97%A5&option=!");
  FormDataParseResult parsed = form_data_parse(form_data_raw, &arena);
  ASSERT(!parsed.err);
  ASSERT(4 == parsed.form.len);

  FormDataKV kv0 = dyn_at(parsed.form, 0);
  FormDataKV kv1 = dyn_at(parsed.form, 1);
  FormDataKV kv2 = dyn_at(parsed.form, 2);
  FormDataKV kv3 = dyn_at(parsed.form, 3);

  ASSERT(string_eq(kv0.key, S("foo")));
  ASSERT(string_eq(kv0.value, S("bar")));

  ASSERT(string_eq(kv1.key, S("name")));
  ASSERT(string_eq(kv1.value, S("hello world")));

  ASSERT(string_eq(kv2.key, S("option")));
  ASSERT(string_eq(kv2.value, S("日")));

  ASSERT(string_eq(kv3.key, S("option")));
  ASSERT(string_eq(kv3.value, S("!")));
}

static void test_json_encode_decode_string_slice() {
  Arena arena = arena_make_from_virtual_mem(4096);

  DynString dyn = {0};
  *dyn_push(&dyn, &arena) = S("hello \"world\n\"!");
  *dyn_push(&dyn, &arena) = S("日");

  String encoded =
      json_encode_string_slice(dyn_slice(StringSlice, dyn), &arena);
  JsonParseStringStrResult decoded = json_decode_string_slice(encoded, &arena);
  ASSERT(!decoded.err);
  ASSERT(decoded.string_slice.len == dyn.len);
  for (uint64_t i = 0; i < dyn.len; i++) {
    String expected = dyn_at(dyn, i);
    String got = AT(decoded.string_slice.data, decoded.string_slice.len, i);
    ASSERT(string_eq(got, expected));
  }
}

static void test_slice_range() {
  Arena arena = arena_make_from_virtual_mem(4096);

  DynString dyn = {0};
  // Works on empty slices.
  (void)slice_range(dyn_slice(StringSlice, dyn), 0, 0);

  *dyn_push(&dyn, &arena) = S("hello \"world\n\"!");
  *dyn_push(&dyn, &arena) = S("日");
  *dyn_push(&dyn, &arena) = S("本語");

  StringSlice slice = dyn_slice(StringSlice, dyn);
  StringSlice range = slice_range(slice, 1, 0);
  ASSERT(2 == range.len);

  ASSERT(string_eq(slice_at(slice, 1), slice_at(range, 0)));
  ASSERT(string_eq(slice_at(slice, 2), slice_at(range, 1)));
}

int main() {
  test_string_indexof_slice();
  test_string_trim();
  test_string_split();
  test_read_http_request_without_body();
  test_read_http_request_with_body();
  test_log_entry_quote_value();
  test_make_log_line();
  test_dyn_ensure_cap();
  test_http_server_post();
  test_http_server_serve_file();
  test_form_data_parse();
  test_json_encode_decode_string_slice();
  test_slice_range();
}
