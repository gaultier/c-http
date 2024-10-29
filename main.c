#include "./sqlite3.h"
#include "http.c"

static sqlite3 *db = nullptr;
static sqlite3_stmt *db_insert_poll_stmt = nullptr;
static sqlite3_stmt *db_select_poll_stmt = nullptr;
static sqlite3_stmt *db_insert_vote_stmt = nullptr;

static const String user_id_cookie_name = S("__Secure-user_id");

typedef enum {
  DB_ERR_NONE,
  DB_ERR_NOT_FOUND,
  DB_ERR_INVALID_USE,
  DB_ERR_INVALID_DATA,
} DatabaseError;

typedef enum : u8 {
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
  POLL_STATE_MAX, // Pseudo-value.
} PollState;

typedef struct {
  i64 db_id;
  String human_readable_id;
  PollState state;
  String name;
  StringSlice options;
  String created_at;
  String created_by;
} Poll;

[[nodiscard]] static HttpResponse
http_response_add_user_id_cookie(HttpResponse resp, String user_id,
                                 Arena *arena) {
  HttpResponse res = resp;

  DynU8 cookie_value = {0};
  dyn_append_slice(&cookie_value, user_id_cookie_name, arena);
  dyn_append_slice(&cookie_value, S("="), arena);
  dyn_append_slice(&cookie_value, user_id, arena);
  dyn_append_slice(&cookie_value, S("; Secure"), arena);
  dyn_append_slice(&cookie_value, S("; HttpOnly"), arena);

  *dyn_push(&res.headers, arena) = (KeyValue){
      .key = S("Set-Cookie"),
      .value = dyn_slice(String, cookie_value),
  };
  return res;
}

[[nodiscard]] static DatabaseError db_create_poll(String req_id, Poll poll,
                                                  Arena *arena) {
  ASSERT(!slice_is_empty(poll.created_by));

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

  if (SQLITE_OK !=
      (db_err = sqlite3_bind_text(db_insert_poll_stmt, 4,
                                  (const char *)poll.created_by.data,
                                  (int)poll.created_by.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 4", arena,
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
  // TODO: Use the same style as the rest of the app?
  res.body = S("<!DOCTYPE html><html><body>Not found.</body></html>");
  return res;
}

[[nodiscard]] static HttpResponse
http_respond_with_internal_server_error(String req_id, Arena *arena) {
  HttpResponse res = {0};
  res.status = 500;

  DynU8 body = {0};
  // TODO: Use the same style as the rest of the app?
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
               .human_readable_id = make_unique_id_u128_string(arena)};

  poll.created_by =
      http_req_extract_cookie_with_name(req, user_id_cookie_name, arena);
  if (slice_is_empty(poll.created_by)) {
    poll.created_by = make_unique_id_u128_string(arena);
    log(LOG_LEVEL_INFO, "generating new user id", arena, L("req.id", req.id),
        L("user_id", poll.created_by));

    res = http_response_add_user_id_cookie(res, poll.created_by, arena);
  }

  {
    FormDataParseResult form = form_data_parse(req.body, arena);
    if (form.err) {
      log(LOG_LEVEL_ERROR, "failed to create poll due to invalid options",
          arena, L("req.id", req.id), L("req.body", req.body));
      return http_respond_with_unprocessable_entity(req.id, arena);
    }

    DynString dyn_options = {0};
    for (u64 i = 0; i < form.form.len; i++) {
      FormDataKV kv = dyn_at(form.form, i);
      String value = html_sanitize(kv.value, arena);

      if (string_eq(kv.key, S("name"))) {
        poll.name = value;
      } else if (string_eq(kv.key, S("option")) && !slice_is_empty(value)) {
        *dyn_push(&dyn_options, arena) = value;
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
    log(LOG_LEVEL_ERROR, "failed to create poll due to invalid db data", arena,
        L("req.id", req.id), L("req.body", req.body));
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
    log(LOG_LEVEL_ERROR, "failed to execute the prepared statement to get poll",
        arena, L("req.id", req_id), L("error", err));
    res.err = DB_ERR_INVALID_USE;
    return res;
  }

  res.poll.db_id = sqlite3_column_int64(db_select_poll_stmt, 0);
  ASSERT(0 != res.poll.db_id);
  res.poll.name.data = (u8 *)sqlite3_column_text(db_select_poll_stmt, 1);
  res.poll.name.len = (u64)sqlite3_column_bytes(db_select_poll_stmt, 1);

  int state = sqlite3_column_int(db_select_poll_stmt, 2);
  if (state >= POLL_STATE_MAX) {
    log(LOG_LEVEL_ERROR, "invalid poll state", arena, L("state", state),
        L("req.id", req_id), L("error", err));
    res.err = DB_ERR_INVALID_DATA;
    return res;
  }
  res.poll.state = (PollState)state;

  String options_json_encoded = {0};
  options_json_encoded.data = (u8 *)sqlite3_column_text(db_select_poll_stmt, 3);
  options_json_encoded.len = (u64)sqlite3_column_bytes(db_select_poll_stmt, 3);

  JsonParseStringStrResult options_decoded =
      json_decode_string_slice(options_json_encoded, arena);
  if (options_decoded.err) {
    log(LOG_LEVEL_ERROR, "invalid poll options", arena, L("req.id", req_id),
        L("options", options_json_encoded), L("error", options_decoded.err));
    res.err = DB_ERR_INVALID_DATA;
    return res;
  }

  res.poll.options = options_decoded.string_slice;

  res.poll.created_at.data = (u8 *)sqlite3_column_text(db_select_poll_stmt, 4);
  res.poll.created_at.len = (u64)sqlite3_column_bytes(db_select_poll_stmt, 4);
  ASSERT(!slice_is_empty(res.poll.created_at));

  res.poll.created_by.data = (u8 *)sqlite3_column_text(db_select_poll_stmt, 5);
  res.poll.created_by.len = (u64)sqlite3_column_bytes(db_select_poll_stmt, 5);
  ASSERT(!slice_is_empty(res.poll.created_by));

  return res;
}

[[nodiscard]] static String make_get_poll_html(Poll poll, String user_id,
                                               Arena *arena) {
  ASSERT(!slice_is_empty(poll.created_by));
  ASSERT(!slice_is_empty(user_id));

  DynU8 resp_body = {0};
  HtmlDocument document = html_make(S("Poll"), arena);
  HtmlElement tag_link_css = {.kind = HTML_LINK};
  {
    *dyn_push(&tag_link_css.attributes, arena) = (KeyValue){
        .key = S("rel"),
        .value = S("stylesheet"),
    };
    *dyn_push(&tag_link_css.attributes, arena) = (KeyValue){
        .key = S("href"),
        .value = S("main.css"),
    };
  }
  *dyn_push(&document.head.children, arena) = tag_link_css;

  {
    HtmlElement body_div = {.kind = HTML_DIV};
    {
      {
        DynU8 text = {0};
        dyn_append_slice(&text, S("The poll \""), arena);
        dyn_append_slice(&text, poll.name, arena);
        dyn_append_slice(&text, S("\" "), arena);

        switch (poll.state) {
        case POLL_STATE_OPEN:
          dyn_append_slice(&text, S("is open."), arena);
          break;
        case POLL_STATE_CLOSED:
          dyn_append_slice(&text, S("is closed."), arena);
          break;
        case POLL_STATE_MAX:
          [[fallthrough]];
        default:
          ASSERT(0);
        }

        *dyn_push(&body_div.children, arena) =
            (HtmlElement){.kind = HTML_TEXT, .text = dyn_slice(String, text)};
      }
      // TODO: Button to close the poll.

      {
        HtmlElement tag_ol = {.kind = HTML_OL};
        for (u64 i = 0; i < poll.options.len; i++) {
          String option = dyn_at(poll.options, i);

          HtmlElement tag_li = {.kind = HTML_LI};
          *dyn_push(&tag_li.children, arena) =
              (HtmlElement){.kind = HTML_TEXT, .text = option};

          HtmlElement tag_button_up = {
              .kind = HTML_BUTTON,
              .text = S("↑"),
          };
          *dyn_push(&tag_li.children, arena) = tag_button_up;

          HtmlElement tag_button_down = {
              .kind = HTML_BUTTON,
              .text = S("↓"),
          };
          *dyn_push(&tag_li.children, arena) = tag_button_down;

          *dyn_push(&tag_ol.children, arena) = tag_li;
        }
        *dyn_push(&body_div.children, arena) = tag_ol;
      }
      {
        HtmlElement created_at_div = {.kind = HTML_DIV};
        DynU8 created_at_text = {0};
        dyn_append_slice(&created_at_text, S("Created at: "), arena);
        dyn_append_slice(&created_at_text, poll.created_at, arena);

        if (string_eq(poll.created_by, user_id)) {
          dyn_append_slice(&created_at_text, S(" by you."), arena);
        } else {
          dyn_append_slice(&created_at_text, S(" by someone else."), arena);
        }

        *dyn_push(&created_at_div.children, arena) = (HtmlElement){
            .kind = HTML_TEXT,
            .text = dyn_slice(String, created_at_text),
        };
        *dyn_push(&body_div.children, arena) = created_at_div;
      }
      *dyn_push(&document.body.children, arena) = body_div;
      *dyn_push(&document.body.children, arena) =
          (HtmlElement){.kind = HTML_DIV};
    }

    html_document_to_string(document, &resp_body, arena);
  }

  return dyn_slice(String, resp_body);
}

[[nodiscard]] static HttpResponse handle_get_poll(HttpRequest req,
                                                  Arena *arena) {
  ASSERT(HM_GET == req.method);
  ASSERT(2 == req.path_components.len);
  String poll_id = dyn_at(req.path_components, 1);
  ASSERT(32 == poll_id.len);

  HttpResponse res = {0};

  DbGetPollResult get_poll = db_get_poll(req.id, poll_id, arena);

  switch (get_poll.err) {
  case DB_ERR_NONE:
    break;
  case DB_ERR_NOT_FOUND:
    return http_respond_with_not_found();
  case DB_ERR_INVALID_USE:
    return http_respond_with_internal_server_error(req.id, arena);
  case DB_ERR_INVALID_DATA:
    return http_respond_with_unprocessable_entity(req.id, arena);
    log(LOG_LEVEL_ERROR, "failed to get poll due to invalid db data", arena,
        L("req.id", req.id), L("req.body", req.body));
  default:
    ASSERT(false);
  }

  String user_id =
      http_req_extract_cookie_with_name(req, user_id_cookie_name, arena);
  if (slice_is_empty(user_id)) {
    user_id = make_unique_id_u128_string(arena);
    log(LOG_LEVEL_INFO, "generating new user id", arena, L("req.id", req.id),
        L("user_id", user_id));

    res = http_response_add_user_id_cookie(res, user_id, arena);
  }

  res.body =
      dyn_slice(String, make_get_poll_html(get_poll.poll, user_id, arena));
  res.status = 200;
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

[[nodiscard]] static DatabaseError
db_cast_vote(String req_id, String human_readable_poll_id, String user_id,
             StringSlice vote_options, Arena *arena) {

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

  // Check that the options sent match the options for the poll.
  {
    if (vote_options.len != get_poll.poll.options.len) {
      return DB_ERR_INVALID_DATA;
    }

    for (u64 i_vote = 0; i_vote < vote_options.len; i_vote++) {
      String vote_option = slice_at(vote_options, i_vote);

      bool found = false;
      for (u64 i_poll = 0; i_poll < get_poll.poll.options.len; i_poll++) {
        String poll_option = slice_at(get_poll.poll.options, i_poll);

        if (string_eq(vote_option, poll_option)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return DB_ERR_INVALID_DATA;
      }
    }
  }

  if (SQLITE_OK !=
      (err = sqlite3_bind_text(db_insert_vote_stmt, 1, (char *)user_id.data,
                               (int)user_id.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 1", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  if (SQLITE_OK !=
      (err = sqlite3_bind_int64(db_insert_vote_stmt, 2, get_poll.poll.db_id))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 2", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

  String poll_options_encoded = json_encode_string_slice(vote_options, arena);
  if (SQLITE_OK !=
      (err = sqlite3_bind_text(db_insert_vote_stmt, 3,
                               (const char *)poll_options_encoded.data,
                               (int)poll_options_encoded.len, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to bind parameter 3", arena,
        L("req.id", req_id), L("error", err));
    return DB_ERR_INVALID_USE;
  }

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
    for (u64 i = 0; i < form.form.len; i++) {
      FormDataKV kv = dyn_at(form.form, i);
      String value = html_sanitize(kv.value, arena);

      if (string_eq(kv.key, S("option")) && !slice_is_empty(value)) {
        *dyn_push(&dyn_options, arena) = value;
      }
      // Ignore unknown form data.
    }

    options = dyn_slice(StringSlice, dyn_options);
  }

  String user_id =
      http_req_extract_cookie_with_name(req, user_id_cookie_name, arena);
  if (slice_is_empty(user_id)) {
    log(LOG_LEVEL_ERROR,
        "failed to create vote due to missing/empty user-agent", arena,
        L("req.id", req.id));
    return http_respond_with_unprocessable_entity(req.id, arena);
  }

  switch (db_cast_vote(req.id, poll_id, user_id, options, arena)) {
  case DB_ERR_NONE:
    break;
  case DB_ERR_NOT_FOUND:
    return http_respond_with_not_found();
  case DB_ERR_INVALID_USE:
    return http_respond_with_internal_server_error(req.id, arena);
  case DB_ERR_INVALID_DATA:
    log(LOG_LEVEL_ERROR, "failed to create vote due to invalid db data", arena,
        L("req.id", req.id), L("req.body", req.body));
    return http_respond_with_unprocessable_entity(req.id, arena);
  default:
    ASSERT(false);
  }

  log(LOG_LEVEL_INFO, "vote was cast", arena, L("req.id", req.id),
      L("poll.id", poll_id));

  res.status = 200;
  // FIXME
  res.body = S("<!DOCTYPE html><html><body>Voted!</body></html>");
  return res;
}

[[nodiscard]] static String make_home_html(Arena *arena) {
  DynU8 res = {0};
  HtmlDocument document = html_make(S("Create a poll"), arena);
  {
    HtmlElement tag_link_css = {.kind = HTML_LINK};
    {
      *dyn_push(&tag_link_css.attributes, arena) = (KeyValue){
          .key = S("rel"),
          .value = S("stylesheet"),
      };
      *dyn_push(&tag_link_css.attributes, arena) = (KeyValue){
          .key = S("href"),
          .value = S("main.css"),
      };
    }
    *dyn_push(&document.head.children, arena) = tag_link_css;
  }
  {
    HtmlElement tag_script = {.kind = HTML_SCRIPT};
    {
      *dyn_push(&tag_script.attributes, arena) = (KeyValue){
          .key = S("src"),
          .value = S("main.js"),
      };
    }
    *dyn_push(&document.head.children, arena) = tag_script;
  }

  {
    HtmlElement tag_form = {.kind = HTML_FORM};
    *dyn_push(&tag_form.attributes, arena) = (KeyValue){
        .key = S("action"),
        .value = S("/poll"),
    };
    *dyn_push(&tag_form.attributes, arena) = (KeyValue){
        .key = S("method"),
        .value = S("post"),
    };

    {
      HtmlElement tag_fieldset = {.kind = HTML_FIELDSET};
      *dyn_push(&tag_fieldset.attributes, arena) = (KeyValue){
          .key = S("id"),
          .value = S("poll-form-fieldset"),
      };

      {
        HtmlElement tag_legend = {.kind = HTML_LEGEND, .text = S("New poll")};
        *dyn_push(&tag_fieldset.children, arena) = tag_legend;
      }
      HtmlElement tag_div_name = {.kind = HTML_DIV};
      {
        HtmlElement tag_label = {.kind = HTML_LABEL};
        HtmlElement text = {.kind = HTML_TEXT, .text = S("Name: ")};
        *dyn_push(&tag_label.children, arena) = text;
        *dyn_push(&tag_div_name.children, arena) = tag_label;
      }
      {
        HtmlElement tag_input = {.kind = HTML_INPUT};
        *dyn_push(&tag_input.attributes, arena) =
            (KeyValue){.key = S("name"), .value = S("name")};
        *dyn_push(&tag_input.attributes, arena) = (KeyValue){
            .key = S("placeholder"), .value = S("Where do we go on vacation?")};
        *dyn_push(&tag_div_name.children, arena) = tag_input;
      }
      *dyn_push(&tag_fieldset.children, arena) = tag_div_name;

      {
        HtmlElement tag_button = {
            .kind = HTML_BUTTON,
            .text = S("+"),
        };
        *dyn_push(&tag_button.attributes, arena) = (KeyValue){
            .key = S("type"),
            .value = S("button"),
        };
        *dyn_push(&tag_button.attributes, arena) = (KeyValue){
            .key = S("id"),
            .value = S("add-poll-option"),
        };
        *dyn_push(&tag_fieldset.children, arena) = tag_button;
      }
      {
        HtmlElement tag_button = {
            .kind = HTML_BUTTON,
            .text = S("Create"),
        };
        *dyn_push(&tag_button.attributes, arena) =
            (KeyValue){.key = S("type"), .value = S("submit")};
        *dyn_push(&tag_fieldset.children, arena) = tag_button;
      }
      *dyn_push(&tag_form.children, arena) = tag_fieldset;
    }

    *dyn_push(&document.body.children, arena) = tag_form;
  }

  html_document_to_string(document, &res, arena);

  return dyn_slice(String, res);
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
    res.body = make_home_html(arena);
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    return res;
  } else if (HM_GET == req.method && 1 == req.path_components.len &&
             string_eq(path0, S("main.css"))) {
    // `GET /main.css`

    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/css"), arena);
    http_response_register_file_for_sending(&res, S("main.css"));
    return res;
  } else if (HM_GET == req.method && 1 == req.path_components.len &&
             string_eq(path0, S("main.js"))) {
    // `GET /main.js`

    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"),
                     S("application/javascript"), arena);
    http_response_register_file_for_sending(&res, S("main.js"));
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
    return handle_cast_vote(req, arena);
  } else {
    return http_respond_with_not_found();
  }
  ASSERT(0);
}

[[nodiscard]] static DatabaseError db_setup(Arena *arena) {
  int db_err = 0;
  if (SQLITE_OK != (db_err = sqlite3_initialize())) {
    log(LOG_LEVEL_ERROR, "failed to initialize sqlite", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

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
  for (u64 i = 0; i < static_array_len(pragmas); i++) {
    if (SQLITE_OK !=
        (db_err = sqlite3_exec(db, AT(pragmas, static_array_len(pragmas), i),
                               nullptr, nullptr, nullptr))) {
      log(LOG_LEVEL_ERROR, "failed to execute pragmas", arena, L("i", i),
          L("error", db_err));
      return DB_ERR_INVALID_USE;
    }
  }

  if (SQLITE_OK !=
      (db_err = sqlite3_exec(
           db,
           "create table if not exists polls (id "
           "integer primary key, name text, "
           "state int, options text, human_readable_id text unique, "
           "created_at text, created_by text) STRICT",
           nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create polls table", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }
  if (SQLITE_OK !=
      (db_err = sqlite3_exec(
           db,
           "create table if not exists votes (id "
           "integer primary key, created_at text, user_id text unique, "
           "poll_id text,"
           "options text,"
           "foreign key(poll_id) references polls(id)"
           ") STRICT",
           nullptr, nullptr, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to create votes table", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_insert_poll_sql = S("insert into polls (human_readable_id, name, "
                                "state, options, created_at, created_by) "
                                "values (?, ?, 0, ?, datetime('now'), ?)");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_insert_poll_sql.data,
                                   (int)db_insert_poll_sql.len,
                                   &db_insert_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to insert poll", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_select_poll_sql = S("select id, name, state, options, created_at, "
                                "created_by from polls where "
                                "human_readable_id = ? limit 1");
  if (SQLITE_OK !=
      (db_err = sqlite3_prepare_v2(db, (const char *)db_select_poll_sql.data,
                                   (int)db_select_poll_sql.len,
                                   &db_select_poll_stmt, nullptr))) {
    log(LOG_LEVEL_ERROR, "failed to prepare statement to select poll", arena,
        L("error", db_err));
    return DB_ERR_INVALID_USE;
  }

  String db_insert_vote_sql = S("insert or replace into votes (created_at, "
                                "user_id, poll_id, options) values "
                                "(datetime('now'), ?, ?, ?)");
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
  Arena arena = arena_make_from_virtual_mem(4 * KiB);

  if (DB_ERR_NONE != db_setup(&arena)) {
    exit(EINVAL);
  }
  ASSERT(nullptr != db);

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              nullptr, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, L("error", err));
}
