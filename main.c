#include "http.c"

static HttpResponse my_http_request_handler(HttpRequest req, Arena *arena) {
  ASSERT(0 == req.err);

  HttpResponse res = {0};
  http_push_header_cstr(&res.headers, "Connection", "close", arena);

  if (HM_GET == req.method &&
      (slice_eq(req.path, S("/")) || slice_eq(req.path, S("/index.html")))) {
    res.status = 200;
    http_push_header_cstr(&res.headers, "Content-Type", "text/html", arena);
    http_response_register_file_for_sending(&res, "index.html");
  } else if (HM_POST == req.method && slice_eq(req.path, S("/comment"))) {
    res.status = 201;
    http_push_header_cstr(&res.headers, "Content-Type", "application/json",
                          arena);
    res.body = S("{\"id\": 1}");
  } else {
    res.status = 404;
  }

  return res;
}

int main() {
  Arena arena = arena_make_from_virtual_mem(4096);
  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, LCI("error", err));
}
