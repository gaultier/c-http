#include "http.c"
#include "submodules/cstd/lib.c"
#include <sys/wait.h>

typedef struct {
  String s;
  u64 idx;
} MemReadContext;

static IoOperationResult reader_read_from_slice(void *ctx, void *buf,
                                                size_t buf_len) {
  MemReadContext *mem_ctx = ctx;

  ASSERT(buf != nullptr);
  ASSERT(mem_ctx->s.data != nullptr);
  if (mem_ctx->idx >= mem_ctx->s.len) {
    // End.
    return (IoOperationResult){0};
  }

  const u64 remaining = mem_ctx->s.len - mem_ctx->idx;
  const u64 can_fill = MIN(remaining, buf_len);
  ASSERT(can_fill <= remaining);

  IoOperationResult res = {
      .s.data = mem_ctx->s.data + mem_ctx->idx,
      .s.len = can_fill,
  };
  memcpy(buf, res.s.data, res.s.len);

  mem_ctx->idx += can_fill;
  ASSERT(mem_ctx->idx <= mem_ctx->s.len);
  ASSERT(res.s.len <= buf_len);
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
  MemReadContext ctx = {.s = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4 * KiB);
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
  MemReadContext ctx = {.s = req_slice};
  Reader reader = reader_make_from_slice(&ctx);
  Arena arena = arena_make_from_virtual_mem(4 * KiB);
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

static u16 random_port() {
  u16 max_port = UINT16_MAX;
  u16 min_port = 3000;

  u16 port = (u16)arc4random_uniform(max_port);
  CLAMP(&port, min_port, max_port);

  return port;
}

static void test_http_server_post() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  // The http server runs in its own child process.
  // The parent process acts as a HTTP client contacting the server.
  u16 port = random_port();

  pid_t pid = fork();
  ASSERT(-1 != pid);
  if (pid == 0) { // Child
    ASSERT(0 == http_server_run(port, handle_request_post, nullptr, &arena));

  } else { // Parent

    for (u64 i = 0; i < 5; i++) {
      HttpRequest req = {
          .method = HM_POST,
          .body = S("foo\nbar"),
      };
      *dyn_push(&req.path_components, &arena) = S("comment");

      http_push_header(&req.headers, S("Content-Type"), S("text/plain"),
                       &arena);
      http_push_header(&req.headers, S("Content-Length"), S("7"), &arena);
      DnsResolveIpv4AddressSocketResult res_resolve =
          net_dns_resolve_ipv4_tcp(S("0.0.0.0"), port, arena);
      ASSERT(!res_resolve.err);
      HttpResponse resp = http_client_request(res_resolve.res, req, &arena);

      if (!resp.err) {
        ASSERT(201 == resp.status);
        ASSERT(string_eq(S("hello world!"), resp.body));
        ASSERT(2 == resp.headers.len);

        KeyValue h1 = dyn_at(resp.headers, 0);
        ASSERT(string_eq(S("Content-Type"), h1.key));
        ASSERT(string_eq(S("text/plain"), h1.value));

        KeyValue h2 = dyn_at(resp.headers, 1);
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
  ASSERT(string_eq(S("/main.css"), req.path_raw) ||
         string_eq(S("/main.css/"), req.path_raw));
  ASSERT(1 == req.path_components.len);
  String path0 = dyn_at(req.path_components, 0);
  ASSERT(string_eq(path0, S("main.css")));

  HttpResponse res = {0};
  res.status = 200;
  http_response_register_file_for_sending(&res, S("main.css"));
  http_push_header(&res.headers, S("Content-Type"), S("text/css"), arena);

  return res;
}

static void test_http_server_serve_file() {
  Arena arena = arena_make_from_virtual_mem(8192);

  // The http server runs in its own child process.
  // The parent process acts as a HTTP client contacting the server.
  u16 port = random_port();

  pid_t pid = fork();
  ASSERT(-1 != pid);
  if (pid == 0) { // Child
    ASSERT(0 == http_server_run(port, handle_request_file, nullptr, &arena));

  } else { // Parent

    for (u64 i = 0; i < 5; i++) {
      HttpRequest req = {
          .method = HM_GET,
      };
      *dyn_push(&req.path_components, &arena) = S("main.css");
      DnsResolveIpv4AddressSocketResult res_resolve =
          net_dns_resolve_ipv4_tcp(S("0.0.0.0"), port, arena);
      ASSERT(!res_resolve.err);
      HttpResponse resp = http_client_request(res_resolve.res, req, &arena);

      if (!resp.err) {
        ASSERT(200 == resp.status);

        ASSERT(2 == resp.headers.len);

        KeyValue h1 = dyn_at(resp.headers, 0);
        ASSERT(string_eq(S("Content-Type"), h1.key));
        ASSERT(string_eq(S("text/css"), h1.value));

        KeyValue h2 = dyn_at(resp.headers, 1);
        ASSERT(string_eq(S("Connection"), h2.key));
        ASSERT(string_eq(S("close"), h2.value));

        int file = open("main.css", O_RDONLY);
        ASSERT(-1 != file);
        struct stat st = {0};
        ASSERT(-1 != stat("main.css", &st));

        ASSERT(st.st_size >= 0);
        ASSERT((u64)st.st_size == resp.body.len);

        void *file_content =
            mmap(nullptr, (u64)st.st_size, PROT_READ, MAP_PRIVATE, file, 0);
        ASSERT(nullptr != file_content);

        String file_content_slice = {.data = file_content,
                                     .len = (u64)st.st_size};
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
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

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
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  DynString dyn = {0};
  *dyn_push(&dyn, &arena) = S("hello \"world\n\"!");
  *dyn_push(&dyn, &arena) = S("日");

  String encoded =
      json_encode_string_slice(dyn_slice(StringSlice, dyn), &arena);
  JsonParseStringStrResult decoded = json_decode_string_slice(encoded, &arena);
  ASSERT(!decoded.err);
  ASSERT(decoded.string_slice.len == dyn.len);
  for (u64 i = 0; i < dyn.len; i++) {
    String expected = dyn_at(dyn, i);
    String got = AT(decoded.string_slice.data, decoded.string_slice.len, i);
    ASSERT(string_eq(got, expected));
  }
}

static void test_html_to_string() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  HtmlDocument document = html_make(S("There and back again"), &arena);
  *dyn_push(&document.body.children, &arena) = (HtmlElement){
      .kind = HTML_LEGEND,
      .text = S("hello world"),
  };

  DynU8 sb = {0};
  html_document_to_string(document, &sb, &arena);
  String s = dyn_slice(String, sb);

  String expected =
      S("<!DOCTYPE html><html><head><meta "
        "charset=\"utf-8\"><title>There and back "
        "again</title></head><body><legend>hello world</legend></body></html>");
  ASSERT(string_eq(expected, s));
}

static void test_extract_user_id_cookie() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  // No `Cookie` header.
  {
    HttpRequest req = {0};
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Host"),
        .value = S("google.com"),
    };

    String value = http_req_extract_cookie_with_name(req, S("foo"), &arena);
    ASSERT(string_eq(S(""), value));
  }
  // `Cookie` header present but with different name.
  {
    HttpRequest req = {0};
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Host"),
        .value = S("google.com"),
    };
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Cookie"),
        .value = S("bar=foo"),
    };

    String value = http_req_extract_cookie_with_name(req, S("foo"), &arena);
    ASSERT(string_eq(S(""), value));
  }
  // `Cookie` header present with matching name but value is empty.
  {
    HttpRequest req = {0};
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Host"),
        .value = S("google.com"),
    };
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Cookie"),
        .value = S("foo="),
    };

    String value = http_req_extract_cookie_with_name(req, S("foo"), &arena);
    ASSERT(string_eq(S(""), value));
  }
  // `Cookie` header present with matching name and value has multiple
  // components.
  {
    HttpRequest req = {0};
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Host"),
        .value = S("google.com"),
    };
    *dyn_push(&req.headers, &arena) = (KeyValue){
        .key = S("Cookie"),
        .value = S("foo=bar; SameSite=Strict; Secure"),
    };

    String value = http_req_extract_cookie_with_name(req, S("foo"), &arena);
    ASSERT(string_eq(S("bar"), value));
  }
}

static void test_html_sanitize() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);
  String s =
      S("<pre onclick=\"alert('hello')\"><code>int main() {}</code></pre>");
  String sanitized = html_sanitize(s, &arena);
  String expected =
      S("&ltpre onclick=&quotalert(&#x27hello&#x27)&quot&gt&ltcode&gtint "
        "main() {}&lt/code&gt&lt/pre&gt");

  ASSERT(string_eq(expected, sanitized));
}

static void test_http_request_serialize() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  HttpRequest req = {0};
  req.method = HM_GET;
  http_push_header(&req.headers, S("Authorization"), S("Bearer abc"), &arena);
  *dyn_push(&req.path_components, &arena) = S("announce");
  *dyn_push(&req.url_parameters, &arena) = (KeyValue){
      .key = S("event"),
      .value = S("started"),
  };
  *dyn_push(&req.url_parameters, &arena) = (KeyValue){
      .key = S("port"),
      .value = S("6883"),
  };

  req.body = S("hello, world!");

  String serialized = http_request_serialize(req, &arena);

  String expected =
      S("GET /announce?event=started&port=6883 HTTP/1.1\r\nAuthorization: "
        "Bearer abc\r\n\r\nhello, world!");

  ASSERT(string_eq(serialized, expected));
}

static void test_url_parse() {
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  {
    ParseUrlResult res = url_parse(S(""), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("x"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http:"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http:/"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("://"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://a:"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://a:/"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://a:bc"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://abc:0"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://abc:999999"), &arena);
    ASSERT(!res.ok);
  }
  {
    ParseUrlResult res = url_parse(S("http://a:80"), &arena);
    ASSERT(res.ok);
    ASSERT(string_eq(S("http"), res.url.scheme));
    ASSERT(0 == res.url.username.len);
    ASSERT(0 == res.url.password.len);
    ASSERT(string_eq(S("a"), res.url.host));
    ASSERT(0 == res.url.path_components.len);
    ASSERT(80 == res.url.port);
  }
  {
    ParseUrlResult res = url_parse(S("http://a.b.c:80/foo"), &arena);
    ASSERT(res.ok);
    ASSERT(string_eq(S("http"), res.url.scheme));
    ASSERT(0 == res.url.username.len);
    ASSERT(0 == res.url.password.len);
    ASSERT(string_eq(S("a.b.c"), res.url.host));
    ASSERT(80 == res.url.port);
    ASSERT(1 == res.url.path_components.len);

    String path_component0 = slice_at(res.url.path_components, 0);
    ASSERT(string_eq(S("foo"), path_component0));
  }
  {
    ParseUrlResult res = url_parse(S("http://a.b.c:80/"), &arena);
    ASSERT(res.ok);
    ASSERT(string_eq(S("http"), res.url.scheme));
    ASSERT(0 == res.url.username.len);
    ASSERT(0 == res.url.password.len);
    ASSERT(string_eq(S("a.b.c"), res.url.host));
    ASSERT(80 == res.url.port);
    ASSERT(0 == res.url.path_components.len);
  }
  {
    ParseUrlResult res = url_parse(S("http://a.b.c/foo/bar/baz"), &arena);
    ASSERT(res.ok);
    ASSERT(string_eq(S("http"), res.url.scheme));
    ASSERT(0 == res.url.username.len);
    ASSERT(0 == res.url.password.len);
    ASSERT(string_eq(S("a.b.c"), res.url.host));
    ASSERT(0 == res.url.port);
    ASSERT(3 == res.url.path_components.len);

    String path_component0 = slice_at(res.url.path_components, 0);
    ASSERT(string_eq(S("foo"), path_component0));

    String path_component1 = slice_at(res.url.path_components, 1);
    ASSERT(string_eq(S("bar"), path_component1));

    String path_component2 = slice_at(res.url.path_components, 2);
    ASSERT(string_eq(S("baz"), path_component2));
  }
}

int main() {
  test_read_http_request_without_body();
  test_read_http_request_with_body();
  test_http_server_post();
  test_http_server_serve_file();
  test_form_data_parse();
  test_json_encode_decode_string_slice();
  test_html_to_string();
  test_extract_user_id_cookie();
  test_html_sanitize();
  test_http_request_serialize();
  test_url_parse();
}
