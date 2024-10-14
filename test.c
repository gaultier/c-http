#include "http.c"

static void test_slice_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == slice_indexof_slice((Slice){}, S("fox"))); }

  // Empty needle.
  { ASSERT(-1 == slice_indexof_slice(S("hello"), (Slice){})); }

  // Not found.
  { ASSERT(-1 == slice_indexof_slice(S("hello world"), S("foobar"))); }

  // Found, one occurence.
  { ASSERT(6 == slice_indexof_slice(S("hello world"), S("world"))); }

  // Found, first occurence.
  { ASSERT(6 == slice_indexof_slice(S("world hello hell"), S("hell"))); }

  // Found, second occurence.
  { ASSERT(10 == slice_indexof_slice(S("hello fox foxy"), S("foxy"))); }

  // Almost found, prefix matches.
  { ASSERT(-1 == slice_indexof_slice(S("hello world"), S("worldly"))); }
}

static IoOperationResult
line_buffered_reader_read_from_slice(void *ctx, void *buf, size_t buf_len) {
  Slice *slice = ctx;

  ASSERT(buf != NULL);
  ASSERT(slice->data != NULL);
  ASSERT(slice->len <= buf_len);

  memcpy(buf, slice->data, slice->len);
  return (IoOperationResult){.bytes_count = slice->len};
}

static LineBufferedReader line_buffered_reader_make_from_slice(Slice *slice) {
  return (LineBufferedReader){
      .ctx = slice,
      .read_fn = line_buffered_reader_read_from_slice,
  };
}

static void test_read_http_request() {
  Slice req_slice = S("GET /foo?bar=2 HTTP/1.1\r\nHost: "
                      "localhost:12345\r\nAccept: */*\r\n\r\n");
  LineBufferedReader reader = line_buffered_reader_make_from_slice(&req_slice);
  Arena arena = arena_make_from_virtual_mem(4096);
  const HttpRequest req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_GET == req.method);
  ASSERT(slice_eq(req.path, S("/foo?bar=2")));

  ASSERT(2 == req.headers.len);
  ASSERT(slice_eq(req.headers.data[0].key, S("Host")));
  ASSERT(slice_eq(req.headers.data[0].value, S("localhost:12345")));
  ASSERT(slice_eq(req.headers.data[1].key, S("Accept")));
  ASSERT(slice_eq(req.headers.data[1].value, S("*/*")));
}

static void test_slice_trim() {
  Slice trimmed = slice_trim(S("   foo "), ' ');
  ASSERT(slice_eq(trimmed, S("foo")));
}

static void test_slice_split() {
  Slice slice = S("hello..world...foobar");
  SplitIterator it = slice_split_it(slice, '.');

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("hello")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("world")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, S("foobar")));
  }

  ASSERT(false == slice_split_next(&it).ok);
  ASSERT(false == slice_split_next(&it).ok);
}

static void test_log_entry_quote_value() {
  Arena arena = arena_make_from_virtual_mem(4096);
  // Noop.
  {
    Slice s = S("hello");
    ASSERT(slice_eq(s, log_entry_quote_value(s, &arena)));
  }
  {
    Slice s = S("{\"id\": 1}");
    Slice expected = S("\"{\\\"id\\\": 1}\"");
    ASSERT(slice_eq(expected, log_entry_quote_value(s, &arena)));
  }
}

static void test_make_log_line() {
  Arena arena = arena_make_from_virtual_mem(4096);

  Slice log_line = make_log_line(S("foobar"), &arena, 2, LCI("num", 42),
                                 LCS("slice", S("hello \"world\"")));

  Slice expected = S("message=foobar num=42 slice=\"hello \\\"world\\\"\\n");
  ASSERT(slice_eq(expected, log_line));
}

int main() {
  test_slice_indexof_slice();
  test_slice_trim();
  test_slice_split();
  test_read_http_request();
  test_log_entry_quote_value();
  test_make_log_line();
}
