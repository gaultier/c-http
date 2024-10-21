#include "http.c"
#include <foundationdb/fdb_c_options.g.h>
#include <stdckdint.h>
#define FDB_API_VERSION 710
#include <foundationdb/fdb_c.h>

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

static void *run_fdb_network(void *) {
  ASSERT(0 == fdb_run_network());
  return nullptr;
}

[[nodiscard]] static Slice db_make_poll_key_from_hex_id(Slice poll_id,
                                                        Arena *arena) {
  DynArrayU8 res = {0};
  dyn_append_slice(&res, S("poll/"), arena);
  dyn_append_slice(&res, poll_id, arena);

  return dyn_array_u8_to_slice(res);
}

[[nodiscard]] static Slice db_make_poll_key_from_id(__uint128_t poll_id,
                                                    Arena *arena) {
  DynArrayU8 res = {0};
  dyn_array_u8_append_u128_hex(&res, poll_id, arena);

  return db_make_poll_key_from_hex_id(dyn_array_u8_to_slice(res), arena);
}

[[nodiscard]] static Slice db_make_poll_key_range_start(Slice poll_id,
                                                        Arena *arena) {
  ASSERT(32 == poll_id.len);

  DynArrayU8 res = {0};
  dyn_append_slice(&res, S("poll/"), arena);
  dyn_append_slice(&res, poll_id, arena);
  dyn_append_slice(&res, S("/"), arena);

  return dyn_array_u8_to_slice(res);
}

[[nodiscard]] static Slice db_make_poll_key_range_end(Slice poll_id,
                                                      Arena *arena) {
  ASSERT(32 == poll_id.len);

  DynArrayU8 res = {0};
  dyn_append_slice(&res, S("poll/"), arena);
  dyn_append_slice(&res, poll_id, arena);
  dyn_append_slice(&res, S("/"), arena);
  *dyn_push(&res, arena) = 0xff;

  return dyn_array_u8_to_slice(res);
}

[[nodiscard]] static Slice db_make_poll_value(Poll poll, Arena *arena) {
  DynArrayU8 res = {0};
  dyn_append_slice(&res, S("state="), arena);
  dyn_array_u8_append_u64(&res, poll.state, arena);
  dyn_append_slice(&res, S("&"), arena);

  dyn_append_slice(&res, S("name="), arena);
  dyn_append_slice(&res, poll.name, arena);

  return dyn_array_u8_to_slice(res);
}

[[nodiscard]] static Slice db_make_poll_option_key(__uint128_t poll_id,
                                                   Slice option, Arena *arena) {
  DynArrayU8 res = {0};
  dyn_append_slice(&res, S("poll/"), arena);
  dyn_array_u8_append_u128_hex(&res, poll_id, arena);
  *dyn_push(&res, arena) = '/';
  dyn_append_slice(&res, option, arena);

  return dyn_array_u8_to_slice(res);
}

typedef struct {
  Poll poll;
  Error err;
} DatabaseDecodePollResult;

[[nodiscard]] static DatabaseDecodePollResult db_decode_poll(Slice s) {
  DatabaseDecodePollResult res = {0};

  SplitIterator split_it_ampersand = slice_split(s, '&');
  // `state=<state>`
  {
    SplitResult kv = slice_split_next(&split_it_ampersand);
    if (!kv.ok) {
      res.err = EINVAL;
      return res;
    }

    SplitIterator split_it_equal = slice_split(kv.slice, '=');
    SplitResult key = slice_split_next(&split_it_equal);
    if (!key.ok) {
      res.err = EINVAL;
      return res;
    }

    SplitResult value = slice_split_next(&split_it_equal);
    if (!value.ok) {
      res.err = EINVAL;
      return res;
    }

    if (1 != value.slice.len) {
      res.err = EINVAL;
      return res;
    }

    uint8_t state = AT(value.slice.data, value.slice.len, 0);
    ASSERT(false == ckd_sub(&state, state, '0'));

    if (state >= POLL_STATE_MAX) {
      res.err = EINVAL;
      return res;
    }
    res.poll.state = state;
  }

  // `name=<name>`
  {
    SplitResult kv = slice_split_next(&split_it_ampersand);
    if (!kv.ok) {
      res.err = EINVAL;
      return res;
    }

    SplitIterator split_it_equal = slice_split(kv.slice, '=');
    SplitResult key = slice_split_next(&split_it_equal);
    if (!key.ok) {
      res.err = EINVAL;
      return res;
    }

    res.poll.name = split_it_equal.slice;
  }

  return res;
}

[[nodiscard]] static HttpResponse
handle_create_poll(HttpRequest req, FDBDatabase *db, Arena *arena) {
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

  __uint128_t poll_id = 0;
  // FIXME: Should be `req.form.id` for idempotency.
  arc4random_buf(&poll_id, sizeof(poll_id));

  FDBTransaction *tx = nullptr;
  fdb_error_t fdb_err = 0;
  if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
    log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }
  ASSERT(nullptr != tx);

  {
    Slice key = db_make_poll_key_from_id(poll_id, arena);
    Slice value = db_make_poll_value(poll, arena);
    fdb_transaction_set(tx, (uint8_t *)key.data, (int)key.len, value.data,
                        (int)value.len);
  }

  // For each poll option: insert `<poll.id>/<option>`.
  for (uint64_t i = 0; i < poll.options.len; i++) {
    Slice option = dyn_at(poll.options, i);

    Slice key = db_make_poll_option_key(poll_id, option, arena);

    // No value.
    fdb_transaction_set(tx, (uint8_t *)key.data, (int)key.len, nullptr, 0);
  }

  FDBFuture *future = fdb_transaction_commit(tx);
  ASSERT(nullptr != future);
  if (0 != (fdb_err = fdb_future_block_until_ready(future))) {
    log(LOG_LEVEL_ERROR, "failed to commit db transaction", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }

  log(LOG_LEVEL_INFO, "created poll", arena, LCII("req.id", req.id),
      LCI("poll.options.len", poll.options.len), LCS("poll.name", poll.name));

  res.status = 301;

  DynArrayU8 redirect = {0};
  dyn_append_slice(&redirect, S("/poll/"), arena);
  dyn_array_u8_append_u128_hex(&redirect, poll_id, arena);

  http_push_header(&res.headers, S("Location"), dyn_array_u8_to_slice(redirect),
                   arena);

  return res;
}

[[nodiscard]] static HttpResponse
handle_get_poll(HttpRequest req, FDBDatabase *db, Arena *arena) {
  ASSERT(2 == req.path_components.len);
  Slice poll_id = dyn_at(req.path_components, 1);
  ASSERT(32 == poll_id.len);

  HttpResponse res = {0};

  FDBTransaction *tx = nullptr;
  fdb_error_t fdb_err = 0;
  if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
    log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
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
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }

  if (0 != (fdb_err = fdb_future_get_error(future_options))) {
    log(LOG_LEVEL_ERROR, "failed to wait for the options future", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }

  fdb_bool_t present = false;
  int value_len = 0;
  const uint8_t *value = nullptr;
  if (0 != (fdb_err = fdb_future_get_value(future_poll, &present, &value,
                                           &value_len))) {
    log(LOG_LEVEL_ERROR, "failed to get the value of the poll future", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
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
        LCII("req.id", req.id), LCI("err", (uint64_t)decoded.err));
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
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
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

  fdb_error_t fdb_err = {0};
  {
    ASSERT(0 == fdb_select_api_version(FDB_API_VERSION));
    ASSERT(0 == fdb_setup_network());
  }
  pthread_t fdb_network_thread = {0};
  pthread_create(&fdb_network_thread, nullptr, run_fdb_network, nullptr);

  FDBDatabase *db = nullptr;
  {
    fdb_err = fdb_create_database("fdb.cluster", &db);
    if (0 != fdb_err) {
      log(LOG_LEVEL_ERROR, "failed to connect to db", arena,
          LCI("err", (uint64_t)fdb_err));
      exit(EINVAL);
    }
    ASSERT(nullptr != db);
  }

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
    return handle_create_poll(req, db, arena);
  } else if (HM_GET == req.method && 2 == req.path_components.len &&
             slice_eq(path0, S("poll")) && 32 == path1.len) {
    return handle_get_poll(req, db, arena);
  } else { // TODO: Vote in poll.
    HttpResponse res = {0};
    res.status = 404;
    return res;
  }
  ASSERT(0);
}

int main() {
  Arena arena = arena_make_from_virtual_mem(4096);

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              nullptr, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, LCI("error", err));
}
