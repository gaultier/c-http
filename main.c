#include "http.c"
#include <pthread.h>

typedef enum : uint8_t {
  POLL_STATE_CREATED,
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
} PollState;

typedef struct {
  PollState state;
  // TODO: creation date, etc.
} Poll;

static HttpResponse my_http_request_handler(HttpRequest req, Arena *arena) {
  ASSERT(0 == req.err);

  HttpResponse res = {0};
  http_push_header(&res.headers, S("Connection"), S("close"), arena);

  // Home page.
  if (HM_GET == req.method &&
      (slice_eq(req.path, S("/")) || slice_eq(req.path, S("/index.html")))) {
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    http_response_register_file_for_sending(&res, "index.html");
  } else if (HM_POST == req.method && slice_eq(req.path, S("/poll"))) {
    // Create poll.

    __uint128_t poll_id = 0;
    // FIXME: Should be `req.form.id` for idempotency.
    arc4random_buf(&poll_id, sizeof(poll_id));

    log(LOG_LEVEL_ERROR, "created poll", arena, LCII("req.id", req.id),
        LCII("poll.id", poll_id));

    res.status = 301;

    DynArrayU8 redirect = {0};
    dyn_append_slice(&redirect, S("/poll/"), arena);
    dyn_array_u8_append_u128_hex(&redirect, poll_id, arena);

    http_push_header(&res.headers, S("Location"),
                     dyn_array_u8_to_slice(redirect), arena);
  } else if (HM_GET == req.method && slice_starts_with(req.path, S("/poll/")) &&
             req.path.len > S("/poll/").len) {
    // Get poll.
    res.status = 200;
    res.body = S("<html>vote!</html>");
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
  } else if (HM_POST == req.method &&
             slice_starts_with(req.path, S("/poll/")) &&
             req.path.len > S("/poll/").len) {
    // Vote for poll.
    res.status = 500; // TODO
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
