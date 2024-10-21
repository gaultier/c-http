#include "http.c"
#include <foundationdb/fdb_c_options.g.h>
#define FDB_API_VERSION 710
#include <foundationdb/fdb_c.h>

typedef enum : uint8_t {
  POLL_STATE_CREATED,
  POLL_STATE_OPEN,
  POLL_STATE_CLOSED,
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

static void destroy_transaction(FDBTransaction **tx) {
  if (nullptr != tx) {
    fdb_transaction_destroy(*tx);
  }
}

static void destroy_future(FDBFuture **future) {
  if (nullptr != future) {
    fdb_future_destroy(*future);
  }
}

static Slice db_make_poll_key(__uint128_t poll_id, Arena *arena) {
  DynArrayU8 res = {0};
  dyn_array_u8_append_u128_hex(&res, poll_id, arena);

  return dyn_array_u8_to_slice(res);
}

static Slice db_make_poll_value(Poll poll, Arena *arena) {
  DynArrayU8 res = {0};
  *dyn_push(&res, arena) = poll.state;
  dyn_append_length_prefixed_slice(&res, poll.name, arena);

  return dyn_array_u8_to_slice(res);
}

static Slice db_make_poll_option_key(__uint128_t poll_id, Slice option,
                                     Arena *arena) {
  DynArrayU8 res = {0};
  dyn_array_u8_append_u128_hex(&res, poll_id, arena);
  *dyn_push(&res, arena) = '/';
  dyn_append_slice(&res, option, arena);

  return dyn_array_u8_to_slice(res);
}

static HttpResponse handle_create_poll(HttpRequest req, FDBDatabase *db,
                                       Arena *arena) {
  HttpResponse res = {0};

  FormDataParseResult form = form_data_parse(req.body, arena);
  if (form.err) {
    res.err = form.err;
    return res;
  }

  Poll poll = {.state = POLL_STATE_CREATED};
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

  [[gnu::cleanup(destroy_transaction)]] FDBTransaction *tx = nullptr;
  fdb_error_t fdb_err = 0;
  if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
    log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }
  ASSERT(nullptr != tx);

  {
    Slice key = db_make_poll_key(poll_id, arena);
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

  [[gnu::cleanup(destroy_future)]] FDBFuture *future =
      fdb_transaction_commit(tx);
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

static HttpResponse handle_get_poll(HttpRequest req, FDBDatabase *db,
                                    Arena *arena) {
  HttpResponse res = {0};

  [[gnu::cleanup(destroy_transaction)]] FDBTransaction *tx = nullptr;
  fdb_error_t fdb_err = 0;
  if (0 != (fdb_err = fdb_database_create_transaction(db, &tx))) {
    log(LOG_LEVEL_ERROR, "failed to create db transaction", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }
  ASSERT(nullptr != tx);

  SplitIterator it = slice_split_it(req.path, '/');
  ASSERT(slice_split_next(&it).ok);
  SplitResult split = slice_split_next(&it);
  if (!split.ok || split.slice.len != 32) {
    res.status = 404;
    return res;
  }

  Slice poll_id = split.slice;

  [[gnu::cleanup(destroy_future)]] FDBFuture *future_poll =
      fdb_transaction_get(tx, poll_id.data, (int)poll_id.len, false);

  DynArrayU8 range_key_start = {0};
  dyn_append_slice(&range_key_start, poll_id, arena);
  *dyn_push(&range_key_start, arena) = '/';

  DynArrayU8 range_key_end = {0};
  dyn_append_slice(&range_key_end, poll_id, arena);
  *dyn_push(&range_key_end, arena) = '/';
  *dyn_push(&range_key_end, arena) = 0xff;

  [[gnu::cleanup(destroy_future)]] FDBFuture *future_options =
      fdb_transaction_get_range(
          tx,
          FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(range_key_start.data,
                                            (int)range_key_start.len),
          FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(range_key_end.data,
                                            (int)range_key_end.len),
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

  if ((uint64_t)value_len <
      (uint64_t)sizeof(PollState) + (uint64_t)sizeof(uint64_t)) {
    log(LOG_LEVEL_ERROR, "invalid size of value for poll", arena,
        LCII("req.id", req.id), LCI("value.len", (uint64_t)value_len));
    res.status = 500;
    return res;
  }

  Poll poll = {0};
  poll.state = AT(value, value_len, 0); // The enum range is checked below.
  memcpy(&poll.name.len, value + 1, sizeof(poll.name.len));
  poll.name.data =
      (uint8_t *)AT_PTR(value, value_len, 1 + sizeof(poll.name.len));

  const FDBKey *db_keys = nullptr;
  int db_keys_len = 0;
  if (0 != (fdb_err = fdb_future_get_key_array(future_options, &db_keys,
                                               &db_keys_len))) {
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
  case POLL_STATE_CREATED:
    dyn_append_slice(&resp_body, S(" was successfully created."), arena);
    break;
  case POLL_STATE_OPEN:
    dyn_append_slice(&resp_body, S(" is open."), arena);
    break;
  case POLL_STATE_CLOSED:
    dyn_append_slice(&resp_body, S(" is closed."), arena);
    break;
  default:
    ASSERT(0);
  }

  dyn_append_slice(&resp_body, S("<br>"), arena);
  for (uint64_t i = 0; i < (uint64_t)db_keys_len; i++) {
    FDBKey db_key = AT(db_keys, db_keys_len, i);
    Slice db_key_s = {.data = (uint8_t *)db_key.key,
                      .len = (uint64_t)db_key.key_length};

    if (slice_is_empty(db_key_s) || db_key_s.len < 32 + 1) {
      continue;
    }

    Slice option = slice_range(db_key_s, 32 + 1, 0);

    // TODO: Better HTML.
    dyn_append_slice(&resp_body, S("<span>"), arena);
    dyn_append_slice(&resp_body, option, arena);
    dyn_append_slice(&resp_body, S("</span>"), arena);
  }

  dyn_append_slice(&resp_body, S("</div></body></html>"), arena);

  res.body = dyn_array_u8_to_slice(resp_body);
  res.status = 200;
  http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);

  return res;
}

static HttpResponse my_http_request_handler(HttpRequest req, void *ctx,
                                            Arena *arena) {
  ASSERT(0 == req.err);
  ASSERT(nullptr != ctx);
  FDBDatabase *db = ctx;
  ASSERT(nullptr != db);

  // Home page.
  if (HM_GET == req.method &&
      (slice_eq(req.path, S("/")) || slice_eq(req.path, S("/index.html")))) {
    HttpResponse res = {0};
    res.status = 200;
    http_push_header(&res.headers, S("Content-Type"), S("text/html"), arena);
    http_response_register_file_for_sending(&res, S("index.html"));
    return res;
  } else if (HM_POST == req.method && slice_eq(req.path, S("/poll"))) {
    return handle_create_poll(req, db, arena);
  } else if (HM_GET == req.method && slice_starts_with(req.path, S("/poll/")) &&
             req.path.len >
                 S("/poll/").len) { // TODO: parse path into components.
    return handle_get_poll(req, db, arena);
  } else if (HM_POST == req.method &&
             slice_starts_with(req.path, S("/poll/")) &&
             req.path.len > S("/poll/").len) {
    // Vote for poll.
    HttpResponse res = {0};
    res.status = 500; // TODO
    return res;
  } else {
    HttpResponse res = {0};
    res.status = 404;
    return res;
  }
  ASSERT(0);
}

int main() {
  Arena arena = arena_make_from_virtual_mem(4096);

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
    log(LOG_LEVEL_ERROR, "failed to connect to db", &arena,
        LCI("err", (uint64_t)fdb_err));
    exit(EINVAL);
  }
  ASSERT(nullptr != db);

  Error err = http_server_run(HTTP_SERVER_DEFAULT_PORT, my_http_request_handler,
                              db, &arena);
  log(LOG_LEVEL_INFO, "http server stopped", &arena, LCI("error", err));
}
