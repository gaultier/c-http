#include "http.c"
#include <sqlite3.h>

typedef enum : uint8_t {
  POLL_STATE_CREATED,
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
} PollState;

typedef struct {
  PollState state;
  // TODO: creation date, etc.
} Poll;

static HttpResponse my_http_request_handler(HttpRequest req, void *ctx,
                                            Arena *arena) {
  ASSERT(0 == req.err);
  ASSERT(nullptr != ctx);
  sqlite3 *db = ctx;

  HttpResponse res = {0};
  http_push_header(&res.headers, S("Connection"), S("close"), arena);

  // Home page.
  if (HM_GET == req.method &&
      (slice_eq(req.path, S("/")) || slice_eq(req.path, S("/index.html")))) {
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    http_response_register_file_for_sending(&res, S("index.html"));
  } else if (HM_POST == req.method && slice_eq(req.path, S("/poll"))) {
    // Create poll.

    __uint128_t poll_id = 0;
    // FIXME: Should be `req.form.id` for idempotency.
    arc4random_buf(&poll_id, sizeof(poll_id));

    // TODO: Move this to `main`?
    int db_err = 0;
    const Slice db_insert_kv_query_str =
        S("INSERT INTO vote (key, value) VALUES (?, ?)");
    sqlite3_stmt *db_insert_kv_query = nullptr;
    if (SQLITE_OK !=
        (db_err = sqlite3_prepare_v2(db, (char *)db_insert_kv_query_str.data,
                                     db_insert_kv_query_str.len,
                                     &db_insert_kv_query, nullptr))) {
      log(LOG_LEVEL_ERROR, "failed to prepare statement", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)db_err));
      res.status = 500;
      return res;
    }
    ASSERT(nullptr != db_insert_kv_query);

    if (SQLITE_OK !=
        (db_err = sqlite3_bind_blob(db_insert_kv_query, 1, &poll_id,
                                    sizeof(poll_id), SQLITE_STATIC))) {
      log(LOG_LEVEL_ERROR, "failed to bind 1", arena, LCII("req.id", req.id),
          LCI("err", (uint64_t)db_err));
      res.status = 500;
      return res;
    }

    Poll poll = {.state = POLL_STATE_CREATED};
    if (SQLITE_OK !=
        (db_err = sqlite3_bind_blob(db_insert_kv_query, 2, &poll, sizeof(poll),
                                    SQLITE_STATIC))) {
      log(LOG_LEVEL_ERROR, "failed to bind 2", arena, LCII("req.id", req.id),
          LCI("err", (uint64_t)db_err));
      res.status = 500;
      return res;
    }

    if (SQLITE_DONE != (db_err = sqlite3_step(db_insert_kv_query))) {
      log(LOG_LEVEL_ERROR, "failed to insert poll", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)db_err));
      res.status = 500;
      return res;
    }

    if (SQLITE_OK != (db_err = sqlite3_finalize(db_insert_kv_query))) {
      log(LOG_LEVEL_ERROR, "failed to finalize prepared statement", arena,
          LCII("req.id", req.id), LCI("err", (uint64_t)db_err));
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

  sqlite3 *db = nullptr;
  int db_err = sqlite3_open("vote.db", &db);
  if (SQLITE_OK != db_err) {
    log(LOG_LEVEL_ERROR, "failed to open db", &arena,
        LCI("err", (uint64_t)db_err));
    exit(EINVAL);
  }

  if (SQLITE_OK != (db_err = sqlite3_exec(
                        db,
                        "CREATE TABLE IF NOT EXISTS vote(key varchar not null "
                        "unique, value varchar)",
                        nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create table", &arena,
        LCI("err", (uint64_t)db_err));
    exit(EINVAL);
  }

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              db, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, LCI("error", err));
}
