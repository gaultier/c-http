#include "http.c"
#define FDB_API_VERSION 710
#include <foundationdb/fdb_c.h>
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

static void *run_fdb_network(void *) {
  ASSERT(0 == fdb_run_network());
  return nullptr;
}

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

    // TODO: Move this to the parent process?
    fdb_error_t fdb_err = {0};
    {
      ASSERT(0 == fdb_select_api_version(FDB_API_VERSION));
      ASSERT(0 == fdb_setup_network());
    }
    pthread_t fdb_network_thread = {0};
    pthread_create(&fdb_network_thread, nullptr, run_fdb_network, nullptr);

    FDBDatabase *db = nullptr;
    fdb_err = fdb_create_database("fdb.cluster", &db);
    if (0 != fdb_err) {
      log(LOG_LEVEL_ERROR, "failed to connect to db", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
      res.status = 500;
      return res;
    }
    ASSERT(nullptr != db);

    __uint128_t poll_id = 0;
    // FIXME: Should be `req.form.id` for idempotency.
    arc4random_buf(&poll_id, sizeof(poll_id));

    FDBTransaction *tx = nullptr;
    if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
      log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
      res.status = 500;
      return res;
    }
    ASSERT(nullptr != tx);

    Poll poll = {.state = POLL_STATE_CREATED};
    fdb_transaction_set(tx, (uint8_t *)&poll_id, sizeof(poll_id),
                        (uint8_t *)&poll, sizeof(poll));

    // TODO: For each poll.candidates: insert `<poll.id>/<candidate>`.

    FDBFuture *fut = fdb_transaction_commit(tx);
    ASSERT(nullptr != fut);
    if (0 != (fdb_err = fdb_future_block_until_ready(fut))) {
      log(LOG_LEVEL_ERROR, "failed to commit db transaction", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
      res.status = 500;
      return res;
    }
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
