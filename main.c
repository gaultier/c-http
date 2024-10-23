#include "http.c"
#include <sqlite3.h>
#include <stdckdint.h>

static sqlite3_stmt *db_insert_poll_stmt = nullptr;
static sqlite3_stmt *db_select_poll_stmt = nullptr;

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

[[nodiscard]] static HttpResponse handle_create_poll(HttpRequest req,
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

  Slice options_encoded = json_encode_array(poll.options, arena);
  if (SQLITE_OK !=
      (db_err = sqlite3_bind_text(db_insert_poll_stmt, 3,
                                  (const char *)options_encoded.data,
                                  (int)options_encoded.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 3", arena,
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

  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_bind_text(db_select_poll_stmt, 1,
                                               (const char *)poll_id.data,
                                               (int)poll_id.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("error", db_err));
    res.status = 500;
    return res;
  }

  db_err = sqlite3_step(db_select_poll_stmt);
  if (SQLITE_DONE == db_err) {
    res.status = 404;
    res.body = S("<!DOCTYPE html><html><body>Poll not found.</body></html>");
    return res;
  }

  if (SQLITE_ROW != db_err) {
    log(LOG_LEVEL_ERROR, "failed to execute the prepared statement", arena,
        L("error", db_err));
    res.status = 500;
    return res;
  }

  Poll poll = {0};
  poll.name.data = (uint8_t *)sqlite3_column_text(db_select_poll_stmt, 0);
  poll.name.len = (uint64_t)sqlite3_column_bytes(db_select_poll_stmt, 0);

  int state = sqlite3_column_int(db_select_poll_stmt, 1);
  if (state >= POLL_STATE_MAX) {
    log(LOG_LEVEL_ERROR, "invalid poll state", arena, L("state", state),
        L("error", db_err));
    res.status = 500;
    return res;
  }
  poll.state = (PollState)state;

  // TODO: get options.
  Slice options_json_encoded = {0};
  options_json_encoded.data =
      (uint8_t *)sqlite3_column_text(db_select_poll_stmt, 2);
  options_json_encoded.len =
      (uint64_t)sqlite3_column_bytes(db_select_poll_stmt, 2);

  JsonParseResult options_parsed =
      json_decode_array(options_json_encoded, arena);
  if (options_parsed.err) {
    log(LOG_LEVEL_ERROR, "invalid poll options", arena,
        L("options", options_json_encoded), L("error", options_parsed.err));
    res.status = 500;
    return res;
  }

  DynArrayU8 resp_body = {0};
  // TODO: Use html builder.
  dyn_append_slice(&resp_body,
                   S("<!DOCTYPE html><html><body><div id=\"poll\">"), arena);
  dyn_append_slice(&resp_body, S("The poll \""), arena);
  dyn_append_slice(&resp_body, poll.name, arena);
  dyn_append_slice(&resp_body, S("\" "), arena);

  switch (poll.state) {
  case POLL_STATE_OPEN:
    dyn_append_slice(&resp_body, S("is open."), arena);
    break;
  case POLL_STATE_CLOSED:
    dyn_append_slice(&resp_body, S("is closed."), arena);
    break;
  case POLL_STATE_MAX:
    [[fallthrough]];
  default:
    ASSERT(0);
  }

  dyn_append_slice(&resp_body, S("<br>"), arena);
  for (uint64_t i = 0; i < options_parsed.array.len; i++) {
    Slice option = dyn_at(options_parsed.array, i);

    // TODO: Better HTML.
    dyn_append_slice(&resp_body, S("<span>"), arena);
    dyn_append_slice(&resp_body, option, arena);
    dyn_append_slice(&resp_body, S("</span><br>"), arena);
  }

  dyn_append_slice(&resp_body, S("</div></body></html>"), arena);

  res.body = dyn_array_u8_to_slice(resp_body);
  res.status = 200;
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

[[nodiscard]] static HttpResponse
my_http_request_handler(HttpRequest req, void *ctx, Arena *arena) {
  ASSERT(0 == req.err);
  (void)ctx;

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
    return handle_create_poll(req, arena);
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
           db,
           "create table if not exists poll (id "
           "varchar(32) primary key, name text, state tinyint, options text)",
           nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create poll table", &arena,
        L("error", db_err));
    exit(EINVAL);
  }

  Slice db_insert_poll_sql =
      S("insert into poll (id, name, state, options) values (?, ?, 0, ?)");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_insert_poll_sql.data,
                                   (int)db_insert_poll_sql.len,
                                   &db_insert_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to insert poll", &arena,
        L("error", db_err));
    exit(EINVAL);
  }

  Slice db_select_poll_sql =
      S("select name, state, options from poll where id = ? limit 1");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_select_poll_sql.data,
                                   (int)db_select_poll_sql.len,
                                   &db_select_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to select poll", &arena,
        L("error", db_err));
    exit(EINVAL);
  }

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              nullptr, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, L("error", err));
}
