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

  // Found, multiple occurences.
  {
    ASSERT(6 == slice_indexof_slice(slice_from_cstr("world hello hell"),
                                    slice_from_cstr("hell")));
  }
}

int main() { test_slice_indexof_slice(); }
