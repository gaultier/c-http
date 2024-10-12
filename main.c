#include "lib.c"

static HttpResponse my_http_request_handler(HttpRequest req, Arena *arena) {
  ASSERT(0 == req.err);

  HttpResponse res = {0};
  http_response_push_header_cstr(&res, "Connection", "close", arena);

  if (HM_GET == req.method &&
      (slice_eq(req.path, slice_make_from_cstr("/")) ||
       slice_eq(req.path, slice_make_from_cstr("/index.html")))) {
    res.status = 200;
    http_response_push_header_cstr(&res, "Content-Type", "text/html", arena);
    http_response_register_file_for_sending(&res, "index.html");
  } else if (HM_POST == req.method) {
    res.status = 201;
    http_response_push_header_cstr(&res, "Content-Type", "application/json",
                                   arena);
    res.body = slice_make_from_cstr("[1,2,3]");
  } else {
    res.status = 404;
  }

  return res;
}

int main() { run(my_http_request_handler); }
