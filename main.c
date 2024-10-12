#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>

#define ASSERT(x)                                                              \
  do {                                                                         \
    if (!(x)) {                                                                \
      abort();                                                                 \
    }                                                                          \
  } while (0)

typedef struct {
  uint8_t *data;
  uint64_t len;
} Slice;

static const Slice NEWLINE = {.data = (uint8_t *)"\r\n", .len = 2};

typedef enum { HM_GET, HM_POST } HttpMethod;

typedef struct {
  Slice path;
  HttpMethod method;
} HttpRequest;

typedef struct {
  uint8_t *start;
  uint8_t *end;
} Arena;

typedef struct {
  uint8_t *data;
  uint64_t len, cap;
} DynArrayU8;

#define dyn_push(s, arena)                                                     \
  ((s)->len >= (s)->cap                                                        \
   ? dyn_grow(s, sizeof(*(s)->data), _Alignof(*(s)->data), arena),             \
   (s)->data + (s)->len++ : (s)->data + (s)->len++)

#define dyn_append_slice(dst, src, arena)                                      \
  do {                                                                         \
    for (uint64_t i = 0; i < src.len; i++) {                                   \
      *dyn_push(dst, arena) = src.data[i];                                     \
    }                                                                          \
  } while (0)

void *arena_alloc(Arena *a, uint64_t size, uint64_t align, uint64_t count) {
  ASSERT(a->start != NULL);

  const uint64_t padding = (int64_t)(-(uint64_t)a->start & (align - 1));
  ASSERT(padding <= align);

  const int64_t available = (uint64_t)a->end - (uint64_t)a->start - padding;
  if (available < 0 || count > available / size) {
    abort();
  }

  void *res = a->start + padding;
  ASSERT(res != NULL);
  ASSERT(res <= (void *)a->end);

  a->start += padding + count * size;
  ASSERT(a->start <= a->end);
  ASSERT((uint64_t)a->start % align == 0); // Aligned.

  return memset(res, 0, count * size);
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

Arena arena_make(uint64_t size) {
  void *ptr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

  if (ptr == NULL) {
    fprintf(stderr, "failed to mmap: %d %s\n", errno, strerror(errno));
    exit(errno);
  }
  return (Arena){.start = ptr, .end = ptr + size};
}

static void dyn_grow(void *slice, uint64_t size, uint64_t align, Arena *a) {
  ASSERT(NULL != slice);

  struct {
    void *data;
    uint64_t len;
    uint64_t cap;
  } replica;

  memcpy(&replica, slice, sizeof(replica));

  replica.cap = replica.cap ? replica.cap : 1;
  void *data = arena_alloc(a, 2 * size, align, replica.cap);
  replica.cap *= 2;
  if (replica.len) {
    memcpy(data, replica.data, size * replica.len);
  }
  replica.data = data;

  ASSERT(NULL != slice);

  memcpy(slice, &replica, sizeof(replica));
}

typedef struct {
  uint64_t buf_idx;
  DynArrayU8 buf;
  int socket;
} LineBufferedReader;

typedef struct {
  Slice line;
  int err;
  bool present;
} LineRead;

static int64_t slice_indexof_byte(Slice haystack, uint8_t needle) {
  if (haystack.data == NULL) {
    return -1;
  }

  if (haystack.len == 0) {
    return -1;
  }

  const uint8_t *res = memchr(haystack.data, needle, haystack.len);
  if (res == NULL) {
    return -1;
  }

  return res - haystack.data;
}

Slice slice_range(Slice src, uint64_t start, uint64_t end) {
  ASSERT(start <= end);
  ASSERT(start <= src.len);
  const uint64_t real_end = end == 0 ? src.len : end;
  ASSERT(real_end <= src.len);

  Slice res = {.data = src.data + start, .len = real_end - start};
  return res;
}

bool slice_eq(Slice a, Slice b) {
  if (a.data == NULL && b.data == NULL && a.len == b.len) {
    return true;
  }
  if (a.data == NULL) {
    return false;
  }
  if (b.data == NULL) {
    return false;
  }

  if (a.len != b.len) {
    return false;
  }

  ASSERT(a.data != NULL);
  ASSERT(b.data != NULL);
  ASSERT(a.len == b.len);

  return memcmp(a.data, b.data, a.len) == 0;
}

static int64_t slice_indexof_slice(Slice haystack, Slice needle) {
  if (haystack.data == NULL) {
    return -1;
  }

  if (haystack.len == 0) {
    return -1;
  }

  if (needle.data == NULL) {
    return -1;
  }

  if (needle.len == 0) {
    return -1;
  }

  if (needle.len > haystack.len) {
    return -1;
  }

  uint64_t haystack_idx = 0;

  for (uint64_t _i = 0; _i < haystack.len; _i++) {
    ASSERT(haystack_idx < haystack.len);
    ASSERT(haystack_idx + needle.len <= haystack.len);

    const Slice to_search = slice_range(haystack, haystack_idx, 0);
    const uint64_t found_idx = slice_indexof_byte(to_search, needle.data[0]);
    ASSERT(found_idx <= to_search.len);
    if (needle.len > to_search.len - found_idx) {
      return -1;
    }

    const Slice found_candidate =
        slice_range(to_search, found_idx, found_idx + needle.len);
    if (slice_eq(found_candidate, needle)) {
      return haystack_idx + found_idx;
    }
    haystack_idx += found_idx + NEWLINE.len;
  }

  return -1;
}

Slice dyn_array_u8_range(DynArrayU8 src, uint64_t start, uint64_t end) {
  Slice src_slice = {.data = src.data, .len = src.len};
  return slice_range(src_slice, start, end);
}

static LineRead line_buffered_reader_consume(LineBufferedReader *reader) {
  LineRead res = {0};

  if (reader->buf_idx >= reader->buf.len) {
    return res;
  }

  const Slice to_search = dyn_array_u8_range(reader->buf, reader->buf_idx, 0);
  const int64_t newline_idx = slice_indexof_slice(to_search, NEWLINE);

  if (newline_idx == -1) {
    return res;
  }

  res.line = dyn_array_u8_range(reader->buf, reader->buf_idx,
                                reader->buf_idx + newline_idx);
  res.present = true;
  reader->buf_idx += newline_idx + NEWLINE.len;

  return res;
}

static LineRead line_buffered_reader_read(LineBufferedReader *reader,
                                          Arena *arena) {
  LineRead line = {0};

  for (uint64_t _i = 0; _i < 10; _i++) {
    line = line_buffered_reader_consume(reader);
    if (line.present) {
      return line;
    }

    uint8_t buf[4096] = {0};
    const ssize_t n_read = recv(reader->socket, buf, sizeof(buf), 0);
    if (n_read == -1) {
      continue;
    }

    const Slice slice = {.data = buf, .len = n_read};
    dyn_append_slice(&reader->buf, slice, arena);
  }
  return line;
}

static HttpRequest request_read(LineBufferedReader *reader, Arena *arena) {
  HttpRequest res = {0};

  line_buffered_reader_read(reader, arena);

  return res;
}

static void handle_connection(int conn_fd) {
  Arena arena = arena_make(4096);
  LineBufferedReader reader = {.socket = conn_fd};
  request_read(&reader, &arena);

  //  const int n_written = send(conn_fd, buf, sizeof(buf), 0);
  //  if (n_written == -1) {
  //    fprintf(stderr, "failed to sendto(2): %s\n", strerror(errno));
  //    exit(errno);
  //  }
}

int main() {
  const uint16_t port = 12345;
  struct sigaction sa = {.sa_flags = SA_NOCLDWAIT};
  int err = 0;
  if (-1 == (err = sigaction(SIGCHLD, &sa, NULL))) {
    fprintf(stderr, "Failed to sigaction(2): err=%s\n", strerror(errno));
    exit(errno);
  }

  const int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd == -1) {
    fprintf(stderr, "Failed to socket(2): %s\n", strerror(errno));
    exit(errno);
  }

  int val = 1;
  if ((err = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &val,
                        sizeof(val))) == -1) {
    fprintf(stderr, "Failed to setsockopt(2): %s\n", strerror(errno));
    exit(errno);
  }
#ifdef __FreeBSD__
  if ((err = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val,
                        sizeof(val))) == -1) {
    fprintf(stderr, "Failed to setsockopt(2): %s\n", strerror(errno));
    exit(errno);
  }
#endif

  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };

  if ((err = bind(sock_fd, (const struct sockaddr *)&addr, sizeof(addr))) ==
      -1) {
    fprintf(stderr, "Failed to bind(2): %s\n", strerror(errno));
    exit(errno);
  }

  if ((err = listen(sock_fd, 16 * 1024)) == -1) {
    fprintf(stderr, "Failed to listen(2): %s\n", strerror(errno));
    exit(errno);
  }

  while (1) {
    const int conn_fd = accept(sock_fd, NULL, 0);
    if (conn_fd == -1) {
      fprintf(stderr, "Failed to accept(2): %s\n", strerror(errno));
      return errno;
    }

    const pid_t pid = fork();
    if (pid == -1) {
      fprintf(stderr, "Failed to fork(2): err=%s\n", strerror(errno));
      exit(errno);
    } else if (pid == 0) { // Child
      handle_connection(conn_fd);
      exit(0);
    } else { // Parent
      // Fds are duplicated by fork(2) and need to be
      // closed by both parent & child
      close(conn_fd);
    }
  }
}
