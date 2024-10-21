#include "http.c"
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
    DynArrayU8 key = {0};
    dyn_array_u8_append_u128_hex(&key, poll_id, arena);

    DynArrayU8 value = {0};
    *dyn_push(&value, arena) = poll.state;
    dyn_append_length_prefixed_slice(&value, poll.name, arena);

    fdb_transaction_set(tx, (uint8_t *)key.data, (int)key.len, value.data,
                        (int)value.len);
  }

  // For each poll option: insert `<poll.id>/<option>`.
  for (uint64_t i = 0; i < poll.options.len; i++) {
    Slice option = dyn_at(poll.options, i);

    DynArrayU8 key = {0};
    dyn_array_u8_append_u128_hex(&key, poll_id, arena);
    *dyn_push(&key, arena) = '/';
    dyn_append_length_prefixed_slice(&key, option, arena);

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

  [[gnu::cleanup(destroy_future)]] FDBFuture *future = fdb_transaction_get(
      tx, (uint8_t *)split.slice.data, (int)split.slice.len, false);
  if (0 != (fdb_err = fdb_future_block_until_ready(future))) {
    log(LOG_LEVEL_ERROR, "failed to wait for the future", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }

  fdb_bool_t present = false;
  int value_len = 0;
  const uint8_t *value = nullptr;
  if (0 !=
      (fdb_err = fdb_future_get_value(future, &present, &value, &value_len))) {
    log(LOG_LEVEL_ERROR, "failed to get the value of future", arena,
        LCII("req.id", req.id), LCI("err", (uint64_t)fdb_err));
    res.status = 500;
    return res;
  }
  if (!present) {
    res.status = 404;
    res.body = S("<html><body>Poll not found.</body></html>");
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
  poll.state = AT(value, value_len, 0); // TODO: check enum range.
  memcpy(&poll.name.len, value + 1, sizeof(poll.name.len));
  // FIXME: need to unnecessarily copy because of const.
  poll.name.data = arena_new(arena, uint8_t, poll.name.len);
  memcpy(poll.name.data, AT_PTR(value, value_len, 1 + sizeof(poll.name.len)),
         (uint64_t)value_len - (1 + sizeof(poll.name.len)));

  DynArrayU8 resp_body = {0};
  // TODO: Use html builder.
  dyn_append_slice(&resp_body, S("<html><body><div id=\"poll\">"), arena);
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
