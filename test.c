#include "lib.c"

static void test_slice_indexof_slice() {
  // Empty haystack.
  { ASSERT(-1 == slice_indexof_slice((Slice){}, slice_from_cstr("fox"))); }

  // Empty needle.
  { ASSERT(-1 == slice_indexof_slice(slice_from_cstr("hello"), (Slice){})); }
}

int main() { test_slice_indexof_slice(); }
