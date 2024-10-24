#include "http.c"
#include <sqlite3.h>
#include <stdckdint.h>

static sqlite3 *db = nullptr;
static sqlite3_stmt *db_insert_poll_stmt = nullptr;
static sqlite3_stmt *db_select_poll_stmt = nullptr;
static sqlite3_stmt *db_insert_vote_stmt = nullptr;

typedef enum {
  DB_ERR_NONE,
  DB_ERR_NOT_FOUND,
  DB_ERR_INVALID_USE,
  DB_ERR_INVALID_DATA,
} DatabaseError;

typedef enum : uint8_t {
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
  POLL_STATE_MAX, // Pseudo-value.
} PollState;

typedef struct {
  int64_t db_id;
  String human_readable_id;
  PollState state;
  String name;
  StringSlice options;
  // TODO: creation date, etc.
} Poll;

[[nodiscard]] static DatabaseError db_create_poll(String req_id, Poll poll,
                                                  Arena *arena) {
  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr,
                                          nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to begin transaction", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_OK !=
      (db_err = sqlite3_bind_text(db_insert_poll_stmt, 1,
                                  (const char *)poll.human_readable_id.data,
                                  (int)poll.human_readable_id.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_OK != (db_err = sqlite3_bind_text(db_insert_poll_stmt, 2,
                                               (const char *)poll.name.data,
                                               (int)poll.name.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 2", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String poll_options_encoded = json_encode_string_slice(poll.options, arena);

  if (SQLITE_OK !=
      (db_err = sqlite3_bind_text(db_insert_poll_stmt, 3,
                                  (const char *)poll_options_encoded.data,
                                  (int)poll_options_encoded.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 3", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_DONE != (db_err = sqlite3_step(db_insert_poll_stmt))) {
    log(LOG_LEVEL_ERROR,
        "failed to execute the prepared statement to insert a poll", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_OK !=
      (db_err = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to commit creating a poll", arena,
        L("req.id", req_id), L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  return DB_ERR_NONE;
}

[[nodiscard]] static HttpResponse http_respond_with_not_found() {
  HttpResponse res = {0};
  res.status = 404;
  res.body = S("<!DOCTYPE html><html><body>Not found.</body></html>");
  return res;
}

[[nodiscard]] static HttpResponse
http_respond_with_internal_server_error(String req_id, Arena *arena) {
  HttpResponse res = {0};
  res.status = 500;

  DynU8 body = {0};
  dyn_append_slice(
      &body,
      S("<!DOCTYPE html><html><body>Internal server error. Request id: "),
      arena);
  dyn_append_slice(&body, req_id, arena);
  dyn_append_slice(&body, S("</body></html>"), arena);
  res.body = dyn_slice(String, body);

  return res;
}

[[nodiscard]] static HttpResponse
http_respond_with_unprocessable_entity(String req_id, Arena *arena) {
  HttpResponse res = {0};
  res.status = 422;

  DynU8 body = {0};
  dyn_append_slice(&body,
                   S("<!DOCTYPE html><html><body>Unprocessable entity. The "
                     "data was likely invalid. Request id: "),
                   arena);
  dyn_append_slice(&body, req_id, arena);
  dyn_append_slice(&body, S("</body></html>"), arena);
  res.body = dyn_slice(String, body);

  return res;
}

[[nodiscard]] static HttpResponse handle_create_poll(HttpRequest req,
                                                     Arena *arena) {
  HttpResponse res = {0};

  Poll poll = {.state = POLL_STATE_OPEN,
               .human_readable_id = make_unique_id(arena)};

  {
    FormDataParseResult form = form_data_parse(req.body, arena);
    if (form.err) {
      log(LOG_LEVEL_ERROR, "failed to create poll due to invalid options",
          arena, L("req.id", req.id), L("req.body", req.body));
      return http_respond_with_unprocessable_entity(req.id, arena);
    }

    DynString dyn_options = {0};
    for (uint64_t i = 0; i < form.form.len; i++) {
      FormDataKV kv = dyn_at(form.form, i);

      if (string_eq(kv.key, S("name"))) {
        poll.name = kv.value;
      } else if (string_eq(kv.key, S("option")) && !slice_is_empty(kv.value)) {
        *dyn_push(&dyn_options, arena) = kv.value;
      }
      // Ignore unknown form data.
    }

    poll.options = dyn_slice(StringSlice, dyn_options);
  }

  switch (db_create_poll(req.id, poll, arena)) {
  case DB_ERR_NONE:
    break;
  case DB_ERR_NOT_FOUND:
    ASSERT(false); // Unreachable.
  case DB_ERR_INVALID_USE:
    return http_respond_with_internal_server_error(req.id, arena);
  case DB_ERR_INVALID_DATA:
    return http_respond_with_unprocessable_entity(req.id, arena);
  default:
    ASSERT(false);
  }

  log(LOG_LEVEL_INFO, "created poll", arena, L("req.id", req.id),
      L("poll.options.len", poll.options.len),
      L("poll.id", poll.human_readable_id), L("poll.name", poll.name));

  res.status = 301;

  DynU8 redirect = {0};
  dyn_append_slice(&redirect, S("/poll/"), arena);
  dyn_append_slice(&redirect, poll.human_readable_id, arena);

  http_push_header(&res.headers, S("Location"), dyn_slice(String, redirect),
                   arena);

  return res;
}

typedef struct {
  DatabaseError err;
  Poll poll;
} DbGetPollResult;

[[nodiscard]] static DbGetPollResult
db_get_poll(String req_id, String human_readable_poll_id, Arena *arena) {
  DbGetPollResult res = {0};

  int err = 0;
  if (SQLITE_OK !=
      (err = sqlite3_bind_text(db_select_poll_stmt, 1,
                               (const char *)human_readable_poll_id.data,
                               (int)human_readable_poll_id.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("req.id", req_id), L("error", res.err));
    res.err = DB_ERR_INVALID_USE;
    return res;
  }

  err = sqlite3_step(db_select_poll_stmt);
  if (SQLITE_DONE == err) {
    res.err = DB_ERR_NOT_FOUND;
    return res;
  }

  if (SQLITE_ROW != err) {
    log(LOG_LEVEL_ERROR, "failed to execute the prepared statement", arena,
        L("req.id", req_id), L("error", err));
    res.err = DB_ERR_INVALID_USE;
    return res;
  }

  res.poll.db_id = sqlite3_column_int64(db_select_poll_stmt, 0);
  ASSERT(0 != res.poll.db_id);
  res.poll.name.data = (uint8_t *)sqlite3_column_text(db_select_poll_stmt, 1);
  res.poll.name.len = (uint64_t)sqlite3_column_bytes(db_select_poll_stmt, 1);

  int state = sqlite3_column_int(db_select_poll_stmt, 2);
  if (state >= POLL_STATE_MAX) {
    log(LOG_LEVEL_ERROR, "invalid poll state", arena, L("state", state),
        L("req.id", req_id), L("error", err));
    res.err = DB_ERR_INVALID_DATA;
    return res;
  }
  res.poll.state = (PollState)state;

  // TODO: get options.
  String options_json_encoded = {0};
  options_json_encoded.data =
      (uint8_t *)sqlite3_column_text(db_select_poll_stmt, 3);
  options_json_encoded.len =
      (uint64_t)sqlite3_column_bytes(db_select_poll_stmt, 3);

  JsonParseStringStrResult options_decoded =
      json_decode_string_slice(options_json_encoded, arena);
  if (options_decoded.err) {
    log(LOG_LEVEL_ERROR, "invalid poll options", arena, L("req.id", req_id),
        L("options", options_json_encoded), L("error", options_decoded.err));
    res.err = DB_ERR_INVALID_DATA;
    return res;
  }

  res.poll.options = options_decoded.string_slice;

  return res;
}

[[nodiscard]] static HttpResponse handle_get_poll(HttpRequest req,
                                                  Arena *arena) {
  ASSERT(HM_GET == req.method);
  ASSERT(2 == req.path_components.len);
  String poll_id = dyn_at(req.path_components, 1);
  ASSERT(32 == poll_id.len);

  HttpResponse res = {0};

  DbGetPollResult poll = db_get_poll(req.id, poll_id, arena);

  switch (poll.err) {
  case DB_ERR_NONE:
    break;
  case DB_ERR_NOT_FOUND:
    return http_respond_with_not_found();
  case DB_ERR_INVALID_USE:
    return http_respond_with_internal_server_error(req.id, arena);
  case DB_ERR_INVALID_DATA:
    return http_respond_with_unprocessable_entity(req.id, arena);
  default:
    ASSERT(false);
  }

  DynU8 resp_body = {0};
  // TODO: Use html builder.
  dyn_append_slice(&resp_body,
                   S("<!DOCTYPE html><html><body><div id=\"poll\">"), arena);
  dyn_append_slice(&resp_body, S("The poll \""), arena);
  dyn_append_slice(&resp_body, poll.poll.name, arena);
  dyn_append_slice(&resp_body, S("\" "), arena);

  switch (poll.poll.state) {
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
  for (uint64_t i = 0; i < poll.poll.options.len; i++) {
    String option = dyn_at(poll.poll.options, i);

    // TODO: Better HTML.
    dyn_append_slice(&resp_body, S("<span>"), arena);
    dyn_append_slice(&resp_body, option, arena);
    dyn_append_slice(&resp_body, S("</span><br>"), arena);
  }

  dyn_append_slice(&resp_body, S("</div></body></html>"), arena);

  res.body = dyn_slice(String, resp_body);
  res.status = 200;
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

[[nodiscard]] static DatabaseError db_cast_vote(String req_id,
                                                String human_readable_poll_id,
                                                StringSlice options,
                                                Arena *arena) {

  int err = 0;
  if (SQLITE_OK !=
      (err = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to begin transaction", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  DbGetPollResult get_poll = db_get_poll(req_id, human_readable_poll_id, arena);
  if (get_poll.err) {
    return get_poll.err;
  }
  ASSERT(0 != get_poll.poll.db_id);

  // TODO: Update vote if it already exists.

  if (SQLITE_OK !=
      (err = sqlite3_bind_int64(db_insert_vote_stmt, 1, get_poll.poll.db_id))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  String poll_options_encoded = json_encode_string_slice(options, arena);
  if (SQLITE_OK !=
      (err = sqlite3_bind_text(db_insert_vote_stmt, 2,
                               (const char *)poll_options_encoded.data,
                               (int)poll_options_encoded.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 2", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  // TODO: Check that the options sent match the options for the poll.
  // Or use foreign keys for that :D

  if (SQLITE_DONE != (err = sqlite3_step(db_insert_vote_stmt))) {
    log(LOG_LEVEL_ERROR,
        "failed to execute the prepared statement to insert a vote", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_OK !=
      (err = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to commit creating a vote", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  return DB_ERR_NONE;
}

[[nodiscard]] static HttpResponse handle_cast_vote(HttpRequest req,
                                                   Arena *arena) {
  ASSERT(HM_POST == req.method);
  ASSERT(3 == req.path_components.len);
  String poll_id = dyn_at(req.path_components, 1);
  ASSERT(32 == poll_id.len);

  HttpResponse res = {0};

  StringSlice options = {0};
  {
    FormDataParseResult form = form_data_parse(req.body, arena);
    if (form.err) {
      log(LOG_LEVEL_ERROR, "failed to create vote due to invalid options",
          arena, L("req.id", req.id), L("req.body", req.body));
      return http_respond_with_unprocessable_entity(req.id, arena);
    }

    DynString dyn_options = {0};
    for (uint64_t i = 0; i < form.form.len; i++) {
      FormDataKV kv = dyn_at(form.form, i);

      if (string_eq(kv.key, S("option")) && !slice_is_empty(kv.value)) {
        *dyn_push(&dyn_options, arena) = kv.value;
      }
      // Ignore unknown form data.
    }

    options = dyn_slice(StringSlice, dyn_options);
  }

  switch (db_cast_vote(req.id, poll_id, options, arena)) {
  case DB_ERR_NONE:
    break;
  case DB_ERR_NOT_FOUND:
    return http_respond_with_not_found();
  case DB_ERR_INVALID_USE:
    return http_respond_with_internal_server_error(req.id, arena);
  case DB_ERR_INVALID_DATA:
    return http_respond_with_unprocessable_entity(req.id, arena);
  default:
    ASSERT(false);
  }

  log(LOG_LEVEL_INFO, "vote was cast", arena, L("req.id", req.id),
      L("poll.id", poll_id));

  res.status = 200;
  return res;
}

[[nodiscard]] static HttpResponse
my_http_request_handler(HttpRequest req, void *ctx, Arena *arena) {
  ASSERT(0 == req.err);
  (void)ctx;

  String path0 = req.path_components.len >= 1 ? dyn_at(req.path_components, 0)
                                              : (String){0};
  String path1 = req.path_components.len >= 2 ? dyn_at(req.path_components, 1)
                                              : (String){0};
  // Home page.
  if (HM_GET == req.method && ((req.path_components.len == 0) ||
                               ((req.path_components.len == 1) &&
                                (string_eq(path0, S("index.html")))))) {
    // `GET /`
    // `GET /index.html`

    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    http_response_register_file_for_sending(&res, S("index.html"));
    return res;
  } else if (HM_GET == req.method && 1 == req.path_components.len &&
             string_eq(path0, S("pure-min.css"))) { // TODO: rm.

    // `GET /pure-min.css`

    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/css"), arena);
    http_response_register_file_for_sending(&res, S("pure-min.css"));
    return res;
  } else if (HM_POST == req.method && 1 == req.path_components.len &&
             string_eq(path0, S("poll"))) {
    // `POST /poll`

    return handle_create_poll(req, arena);
  } else if (HM_GET == req.method && 2 == req.path_components.len &&
             string_eq(path0, S("poll")) && 32 == path1.len) {
    // `GET /poll/<poll_id>`

    return handle_get_poll(req, arena);
  } else if (HM_POST == req.method && 3 == req.path_components.len &&
             string_eq(path0, S("poll")) && 32 == path1.len) {
    // `POST /poll/<poll_id>/vote`
    // TODO: user id.
    return handle_cast_vote(req, arena);
  } else {
    return http_respond_with_not_found();
  }
  ASSERT(0);
}

[[nodiscard]] static DatabaseError db_setup(Arena *arena) {
  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_open("vote.db", &db))) {
    log(LOG_LEVEL_ERROR, "failed to open db", arena, L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  // See https://kerkour.com/sqlite-for-servers.
  char *pragmas[] = {
      "PRAGMA journal_mode = WAL",   "PRAGMA busy_timeout = 5000",
      "PRAGMA synchronous = NORMAL", "PRAGMA cache_size = 1000000000",
      "PRAGMA foreign_keys = true",  "PRAGMA temp_store = memory",
  };
  for (uint64_t i = 0; i < static_array_len(pragmas); i++) {
    if (SQLITE_OK !=
        (db_err = sqlite3_exec(db, AT(pragmas, static_array_len(pragmas), i),
                               nullptr, nullptr, nullptr))) {
      log(LOG_LEVEL_ERROR, "failed to execute pragmas", arena, L("i", i),
          L("error", db_err));
      return DB_ERR_INVALID_USE;
    }
  }

  if (SQLITE_OK !=
      (db_err = sqlite3_exec(db,
                             "create table if not exists polls (id "
                             "integer primary key, name text, "
                             "state int, options text, human_readable_id text, "
                             "created_at text) STRICT",
                             nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create polls table", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }
  if (SQLITE_OK != (db_err = sqlite3_exec(
                        db,
                        "create table if not exists votes (id "
                        "integer primary key, created_at text, user_id text, "
                        "poll_id text,"
                        "options text,"
                        "foreign key(poll_id) references polls(id)"
                        ") STRICT",
                        nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create votes table", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_insert_poll_sql =
      S("insert into polls (human_readable_id, name, "
        "state, options, created_at) values (?, ?, 0, ?, datetime('now'))");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_insert_poll_sql.data,
                                   (int)db_insert_poll_sql.len,
                                   &db_insert_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to insert poll", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_select_poll_sql =
      S("select id, name, state, options from polls where "
        "human_readable_id = ? limit 1");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_select_poll_sql.data,
                                   (int)db_select_poll_sql.len,
                                   &db_select_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to select poll", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_insert_vote_sql =
      S("insert into votes (created_at, user_id, poll_id, options) values "
        "(datetime('now'), 'fixme', ?, ?)");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_insert_vote_sql.data,
                                   (int)db_insert_vote_sql.len,
                                   &db_insert_vote_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to insert vote", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  ASSERT(nullptr != db);
  ASSERT(nullptr != db_insert_poll_stmt);
  ASSERT(nullptr != db_select_poll_stmt);
  ASSERT(nullptr != db_insert_vote_stmt);

  return DB_ERR_NONE;
}

int main() {
  Arena arena = arena_make_from_virtual_mem(4096);

  if (DB_ERR_NONE != db_setup(&arena)) {
    exit(EINVAL);
  }
  ASSERT(nullptr != db);

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              nullptr, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, L("error", err));
}
