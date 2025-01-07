#ifndef CHTTP_HTTP_C
#define CHTTP_HTTP_C

#include "submodules/cstd/lib.c"
#include <arpa/inet.h>
#include <asm-generic/errno.h>
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

static const u64 HTTP_SERVER_HANDLER_MEM_LEN = 12 * KiB;
[[maybe_unused]]
static const u16 HTTP_SERVER_DEFAULT_PORT = 12345;
static const int TCP_LISTEN_BACKLOG = 16384;

[[nodiscard]] static Error response_write(Writer *writer, HttpResponse res,
                                          Arena *arena) {
  // Invalid to both want to serve a file and a body.
  ASSERT(slice_is_empty(res.file_path) || slice_is_empty(res.body));

  DynU8 sb = {0};

  dyn_append_slice(&sb, S("HTTP/1.1 "), arena);
  dynu8_append_u64_to_string(&sb, res.status, arena);
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

  Error err = writer_write_all_sync(writer, s);
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

    err = os_sendfile(file_fd, writer->fd, (u64)st.st_size);
    if (err) {
      return err;
    }
  }

  return 0;
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
  BufferedReader reader = buffered_reader_make(socket, &arena);
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

  Writer writer = {.fd = socket};
  Error err = response_write(&writer, res, &arena);
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

[[maybe_unused]] [[nodiscard]] static HttpResponse
http_client_request(Ipv4AddressSocket sock, HttpRequest req, Arena *arena) {
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

  String http_request_serialized = http_request_serialize(req, arena);
  log(LOG_LEVEL_DEBUG, "http request", arena, L("ip", sock.address.ip),
      L("port", sock.address.port), L("serialized", http_request_serialized));

  // TODO: should not be an assert but a returned error.
  ASSERT(send(sock.socket, http_request_serialized.data,
              http_request_serialized.len,
              0) == (i64)http_request_serialized.len);

  BufferedReader reader = buffered_reader_make(sock.socket, arena);

  {
    IoResult io_result =
        buffered_reader_read_until_slice(&reader, S("\r\n"), arena);
    if (io_result.err) {
      res.err = io_result.err;
      goto end;
    }
    if (slice_is_empty(io_result.res)) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    String http1_1_version_needle = S("HTTP/1.1 ");
    String http1_0_version_needle = S("HTTP/1.0 ");
    ASSERT(http1_0_version_needle.len == http1_1_version_needle.len);

    if (!(string_starts_with(io_result.res, http1_0_version_needle) ||
          string_starts_with(io_result.res, http1_1_version_needle))) {
      res.err = HS_ERR_INVALID_HTTP_RESPONSE;
      goto end;
    }

    String status_str =
        slice_range(io_result.res, http1_1_version_needle.len, 0);
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
  IoResult body = buffered_reader_read_until_end(&reader, arena);
  if (body.err) {
    log(LOG_LEVEL_ERROR, "http request failed to read body", arena,
        L("req.method", req.method), L("req.path_raw", req.path_raw),
        L("err", body.err));
    res.err = body.err;
    goto end;
  }

  res.body = body.res;

end:
  (void)net_close_socket(sock.socket);
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

#endif
