#include "lib.c"

static void test_slice_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == slice_indexof_slice((Slice){}, slice_make_from_cstr("fox"))); }

  // Empty needle.
  {
    ASSERT(-1 == slice_indexof_slice(slice_make_from_cstr("hello"), (Slice){}));
  }

  // Not found.
  {
    ASSERT(-1 == slice_indexof_slice(slice_make_from_cstr("hello world"),
                                     slice_make_from_cstr("foobar")));
  }

  // Found, one occurence.
  {
    ASSERT(6 == slice_indexof_slice(slice_make_from_cstr("hello world"),
                                    slice_make_from_cstr("world")));
  }

  // Found, first occurence.
  {
    ASSERT(6 == slice_indexof_slice(slice_make_from_cstr("world hello hell"),
                                    slice_make_from_cstr("hell")));
  }

  // Found, second occurence.
  {
    ASSERT(10 == slice_indexof_slice(slice_make_from_cstr("hello fox foxy"),
                                     slice_make_from_cstr("foxy")));
  }

  // Almost found, prefix matches.
  {
    ASSERT(-1 == slice_indexof_slice(slice_make_from_cstr("hello world"),
                                     slice_make_from_cstr("worldly")));
  }
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
  Slice req_slice =
      slice_make_from_cstr("GET /foo?bar=2 HTTP/1.1\r\nHost: "
                           "localhost:12345\r\nAccept: */*\r\n\r\n");
  LineBufferedReader reader = line_buffered_reader_make_from_slice(&req_slice);
  Arena arena = arena_make(4096);
  const HttpRequestRead req = request_read(&reader, &arena);

  ASSERT(reader.buf_idx == req_slice.len); // Read all.
  ASSERT(0 == req.err);
  ASSERT(HM_GET == req.method);
  ASSERT(slice_eq(req.path, slice_make_from_cstr("/foo?bar=2")));

  ASSERT(2 == req.headers.len);
  ASSERT(slice_eq(req.headers.data[0].key, slice_make_from_cstr("Host")));
  ASSERT(slice_eq(req.headers.data[0].value,
                  slice_make_from_cstr("localhost:12345")));
  ASSERT(slice_eq(req.headers.data[1].key, slice_make_from_cstr("Accept")));
  ASSERT(slice_eq(req.headers.data[1].value, slice_make_from_cstr("*/*")));
}

static void test_slice_trim() {
  Slice trimmed = slice_trim(slice_make_from_cstr("   foo "), ' ');
  ASSERT(slice_eq(trimmed, slice_make_from_cstr("foo")));
}

static void test_slice_split() {
  Slice slice = slice_make_from_cstr("hello..world...foobar");
  SplitIterator it = slice_split_it(slice, '.');

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, slice_make_from_cstr("hello")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, slice_make_from_cstr("world")));
  }

  {
    SplitResult elem = slice_split_next(&it);
    ASSERT(true == elem.ok);
    ASSERT(slice_eq(elem.slice, slice_make_from_cstr("foobar")));
  }

  ASSERT(false == slice_split_next(&it).ok);
  ASSERT(false == slice_split_next(&it).ok);
}

int main() {
  test_slice_indexof_slice();
  test_slice_trim();
  test_slice_split();
  test_read_http_request();
}