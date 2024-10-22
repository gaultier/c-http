#include "http.c"
#include <sqlite3.h>
#include <stdckdint.h>

typedef enum : uint8_t {
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
  POLL_STATE_MAX, // Pseudo-value.
} PollState;

typedef struct {
  PollState state;
  Slice name;
  DynArraySlice options;
  // TODO: creation date, etc.
} Poll;

[[nodiscard]] static Slice make_unique_id(Arena *arena) {
  __uint128_t poll_id = 0;
  // NOTE: It's not idempotent since the client did not provided the id.
  // But in our case it's fine (random id + optional, non-unique name).
  arc4random_buf(&poll_id, sizeof(poll_id));

  DynArrayU8 sb = {0};
  dyn_array_u8_append_u128_hex(&sb, poll_id, arena);

  return dyn_array_u8_to_slice(sb);
}

[[nodiscard]] static HttpResponse
handle_create_poll(HttpRequest req, sqlite3_stmt *db_insert_poll_stmt,
                   Arena *arena) {
  HttpResponse res = {0};

  FormDataParseResult form = form_data_parse(req.body, arena);
  if (form.err) {
    res.err = form.err;
    return res;
  }

  Poll poll = {.state = POLL_STATE_OPEN};
  {
    for (uint64_t i = 0; i < form.form.len; i++) {
      FormDataKV kv = dyn_at(form.form, i);
      if (slice_eq(kv.key, S("name"))) {
        poll.name = kv.value;
      } else if (slice_eq(kv.key, S("option")) && !slice_is_empty(kv.value)) {
        *dyn_push(&poll.options, arena) = kv.value;
      }
      // Ignore unknown form data.
      // TODO: Should it be an error?
    }
  }

  Slice poll_id = make_unique_id(arena);

  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_bind_text(db_insert_poll_stmt, 1,
                                               (const char *)poll_id.data,
                                               (int)poll_id.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("error", db_err));
    res.status = 500;
    return res;
  }
  if (SQLITE_OK != (db_err = sqlite3_bind_text(db_insert_poll_stmt, 2,
                                               (const char *)poll.name.data,
                                               (int)poll.name.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 2", arena,
        L("error", db_err));
    res.status = 500;
    return res;
  }

  if (SQLITE_DONE != (db_err = sqlite3_step(db_insert_poll_stmt))) {
    log(LOG_LEVEL_ERROR, "failed to execute the prepared statement", arena,
        L("error", db_err));
    res.status = 500;
    return res;
  }

  // TODO: For each poll option: insert `<poll.id>/<option>`.

  log(LOG_LEVEL_INFO, "created poll", arena, L("req.id", req.id),
      L("poll.options.len", poll.options.len), L("poll.id", poll_id),
      L("poll.name", poll.name));

  res.status = 301;

  DynArrayU8 redirect = {0};
  dyn_append_slice(&redirect, S("/poll/"), arena);
  dyn_append_slice(&redirect, poll_id, arena);

  http_push_header(&res.headers, S("Location"), dyn_array_u8_to_slice(redirect),
                   arena);

  return res;
}

[[nodiscard]] static HttpResponse handle_get_poll(HttpRequest req,
                                                  Arena *arena) {
  ASSERT(2 == req.path_components.len);
  Slice poll_id = dyn_at(req.path_components, 1);
  ASSERT(32 == poll_id.len);

  HttpResponse res = {0};

#if 0
  FDBTransaction *tx = nullptr;
  fdb_error_t fdb_err = 0;
  if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
    log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
        L("req.id", req.id), L("err", fdb_err));
    res.status = 500;
    return res;
  }
  ASSERT(nullptr != tx);

  Slice poll_key = db_make_poll_key_from_hex_id(poll_id, arena);
  FDBFuture *future_poll =
      fdb_transaction_get(tx, poll_key.data, (int)poll_key.len, false);

  Slice range_key_start = db_make_poll_key_range_start(poll_id, arena);
  Slice range_key_end = db_make_poll_key_range_end(poll_id, arena);

  FDBFuture *future_options = fdb_transaction_get_range(
      tx,
      FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(range_key_start.data,
                                        (int)range_key_start.len),
      FDB_KEYSEL_FIRST_GREATER_THAN(range_key_end.data, (int)range_key_end.len),
      32, 0, FDB_STREAMING_MODE_WANT_ALL, 0, 0, 0);

  for (uint64_t i = 0; i < 1000; i++) {
    if (fdb_future_is_ready(future_poll) &&
        fdb_future_is_ready(future_options)) {
      break;
    }

    usleep(1'000); // 1 ms.
  }

  if (0 != (fdb_err = fdb_future_get_error(future_poll))) {
    log(LOG_LEVEL_ERROR, "failed to wait for the poll future", arena,
        L("req.id", req.id), L("err", fdb_err));
    res.status = 500;
    return res;
  }

  if (0 != (fdb_err = fdb_future_get_error(future_options))) {
    log(LOG_LEVEL_ERROR, "failed to wait for the options future", arena,
        L("req.id", req.id), L("err", fdb_err));
    res.status = 500;
    return res;
  }

  fdb_bool_t present = false;
  int value_len = 0;
  const uint8_t *value = nullptr;
  if (0 != (fdb_err = fdb_future_get_value(future_poll, &present, &value,
                                           &value_len))) {
    log(LOG_LEVEL_ERROR, "failed to get the value of the poll future", arena,
        L("req.id", req.id), L("err", fdb_err));
    res.status = 500;
    return res;
  }
  if (!present) {
    res.status = 404;
    res.body = S("<!DOCTYPE html><html><body>Poll not found.</body></html>");
    return res;
  }

  Slice value_slice = {.data = (uint8_t *)value, .len = (uint64_t)value_len};
  DatabaseDecodePollResult decoded = db_decode_poll(value_slice);
  if (decoded.err) {
    log(LOG_LEVEL_ERROR, "failed to decode the poll from the db", arena,
        L("req.id", req.id), L("err", decoded.err));
    res.err = decoded.err;
    return res;
  }

  Poll poll = decoded.poll;

  const FDBKeyValue *db_keys = nullptr;
  int db_keys_len = 0;
  fdb_bool_t more = false;
  if (0 != (fdb_err = fdb_future_get_keyvalue_array(future_options, &db_keys,
                                                    &db_keys_len, &more))) {
    log(LOG_LEVEL_ERROR, "failed to get the value of the options future", arena,
        L("req.id", req.id), L("err", fdb_err));
    res.status = 500;
    return res;
  }
#endif

  DynArrayU8 resp_body = {0};
  // TODO: Use html builder.
  dyn_append_slice(&resp_body,
                   S("<!DOCTYPE html><html><body><div id=\"poll\">"), arena);
#if 0
  dyn_append_slice(&resp_body, S("The poll \""), arena);
  dyn_append_slice(&resp_body, poll.name, arena);
  dyn_append_slice(&resp_body, S("\" "), arena);

  switch (poll.state) {
  case POLL_STATE_OPEN:
    dyn_append_slice(&resp_body, S(" is open."), arena);
    break;
  case POLL_STATE_CLOSED:
    dyn_append_slice(&resp_body, S(" is closed."), arena);
    break;
  case POLL_STATE_MAX:
    [[fallthrough]];
  default:
    ASSERT(0);
  }

  dyn_append_slice(&resp_body, S("<br>"), arena);
  for (uint64_t i = 0; i < (uint64_t)db_keys_len; i++) {
    FDBKeyValue db_key = AT(db_keys, db_keys_len, i);
    Slice db_key_s = {.data = (uint8_t *)db_key.key,
                      .len = (uint64_t)db_key.key_length};

    if (slice_is_empty(db_key_s) || db_key_s.len < poll_key.len) {
      continue;
    }

    Slice option = slice_range(db_key_s, poll_key.len + 1, 0);

    // TODO: Better HTML.
    dyn_append_slice(&resp_body, S("<span>"), arena);
    dyn_append_slice(&resp_body, option, arena);
    dyn_append_slice(&resp_body, S("</span><br>"), arena);
  }
#endif

  dyn_append_slice(&resp_body, S("</div></body></html>"), arena);

  res.body = dyn_array_u8_to_slice(resp_body);
  res.status = 200;
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

[[nodiscard]] static HttpResponse
my_http_request_handler(HttpRequest req, void *ctx, Arena *arena) {
  ASSERT(0 == req.err);
  sqlite3_stmt *db_insert_poll_stmt = ctx;

  Slice path0 = req.path_components.len >= 1 ? dyn_at(req.path_components, 0)
                                             : (Slice){0};
  Slice path1 = req.path_components.len >= 2 ? dyn_at(req.path_components, 1)
                                             : (Slice){0};
  // Home page.
  if (HM_GET == req.method && ((req.path_components.len == 0) ||
                               ((req.path_components.len == 1) &&
                                (slice_eq(path0, S("index.html")))))) {
    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    http_response_register_file_for_sending(&res, S("index.html"));
    return res;
  } else if (HM_GET == req.method && 1 == req.path_components.len &&
             slice_eq(path0, S("pure-min.css"))) {
    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/css"), arena);
    http_response_register_file_for_sending(&res, S("pure-min.css"));
    return res;
  } else if (HM_POST == req.method && 1 == req.path_components.len &&
             slice_eq(path0, S("poll"))) {
    return handle_create_poll(req, db_insert_poll_stmt, arena);
  } else if (HM_GET == req.method && 2 == req.path_components.len &&
             slice_eq(path0, S("poll")) && 32 == path1.len) {
    return handle_get_poll(req, arena);
  } else { // TODO: Vote in poll.
    HttpResponse res = {0};
    res.status = 404;
    return res;
  }
  ASSERT(0);
}

int main() {
  Arena arena = arena_make_from_virtual_mem(4096);

  sqlite3 *db = nullptr;
  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_open("vote.db", &db))) {
    log(LOG_LEVEL_ERROR, "failed to open db", &arena, L("error", db_err));
    exit(EINVAL);
  }

  if (SQLITE_OK !=
      (db_err = sqlite3_exec(
           db, "create table if not exists poll (id varchar, name varchar)",
           nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create poll tables", &arena,
        L("error", db_err));
    exit(EINVAL);
  }

  sqlite3_stmt *db_insert_poll_stmt = nullptr;
  Slice db_insert_poll_sql = S("insert into poll (id, name) values (?, ?)");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_insert_poll_sql.data,
                                   (int)db_insert_poll_sql.len,
                                   &db_insert_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement", &arena,
        L("error", db_err));
    exit(EINVAL);
  }

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              db_insert_poll_stmt, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, L("error", err));
}
