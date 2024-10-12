#include "lib.c"

static HttpResponse my_http_request_handler(HttpRequest req, Arena *arena) {
  ASSERT(0 == req.err);

  HttpResponse res = {0};
  http_response_push_header_cstr(&res, "Connection", "close", arena);

  if (HM_GET == req.method &&
      (slice_eq(req.path, slice_make_from_cstr("/")) ||
       slice_eq(req.path, slice_make_from_cstr("/index.html")))) {
    res.status = 200; // Dummy status code.
    http_response_push_header_cstr(&res, "Content-Type", "text/html", arena);
  } else {
    res.status = 404;
  }

  return res;
}

int main() { run(my_http_request_handler); }
