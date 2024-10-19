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
  } else if (HM_POST == req.method && slice_eq(req.path, S("/poll"))) {
    res.status = 301;

    __uint128_t poll_id = 0;
    arc4random_buf(&poll_id, sizeof(poll_id));
    DynArrayU8 redirect = {0};
    dyn_append_slice(&redirect, S("/poll/"), arena);
    dyn_array_u8_append_u128_hex(&redirect, poll_id, arena);

    http_push_header(&res.headers, S("Location"),
                     dyn_array_u8_to_slice(redirect), arena);
  } else if (HM_GET == req.method && slice_starts_with(req.path, S("/poll/")) &&
             req.path.len > S("/poll/").len) {
    res.status = 200;
    res.body = S("<html>vote!</html>");
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
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
