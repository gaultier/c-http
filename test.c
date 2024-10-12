#include "lib.c"

static void test_slice_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == slice_indexof_slice((Slice){}, slice_from_cstr("fox"))); }

  // Empty needle.
  { ASSERT(-1 == slice_indexof_slice(slice_from_cstr("hello"), (Slice){})); }

  // Not found.
  {
    ASSERT(-1 == slice_indexof_slice(slice_from_cstr("hello world"),
                                     slice_from_cstr("foobar")));
  }

  // Found, one occurence.
  {
    ASSERT(6 == slice_indexof_slice(slice_from_cstr("hello world"),
                                    slice_from_cstr("world")));
  }

  // Found, first occurence.
  {
    ASSERT(6 == slice_indexof_slice(slice_from_cstr("world hello hell"),
                                    slice_from_cstr("hell")));
  }

  // Found, second occurence.
  {
    ASSERT(10 == slice_indexof_slice(slice_from_cstr("hello fox foxy"),
                                     slice_from_cstr("foxy")));
  }

  // Almost found, prefix matches.
  {
    ASSERT(-1 == slice_indexof_slice(slice_from_cstr("hello world"),
                                     slice_from_cstr("worldly")));
  }
}

static void test_() {

  int main() { test_slice_indexof_slice(); }
