#define _POSIX_C_SOURCE 200809L
#define __XSI_VISIBLE 600
#define __BSD_VISIBLE 1
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
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

void *arena_alloc(Arena *a, uint64_t size, uint64_t align, uint64_t count) {
  ASSERT(a->start != NULL);

  const uint64_t padding = -(uint64_t)a->start & (align - 1);
  const int64_t available = (uint64_t)a->end - (uint64_t)a->start - padding;
  if (available < 0 || count > available / size) {
    abort();
  }

  void *res = a->start + padding;
  ASSERT(res != NULL);
  ASSERT(res <= (void *)a->end);

  a->start += padding + count * size;

  return memset(res, 0, count * size);
}

#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

Arena arena_make(uint64_t size) {
  void *ptr = mmap(NULL, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);

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

static HttpRequest request_read() {
  HttpRequest res = {0};
  return res;
}

static void handle_connection(int conn_fd) {
  Arena arena = arena_make(4096);
  LineBufferedReader reader = {.socket = conn_fd};
  *dyn_push(&reader.buf, &arena) = 42;

  uint8_t buf[1024] = {0};
  const int n_read = recv(conn_fd, buf, sizeof(buf), 0);
  if (n_read == -1) {
    fprintf(stderr, "failed to recvfrom(2): %s\n", strerror(errno));
    exit(errno);
  }

  const int n_written = send(conn_fd, buf, sizeof(buf), 0);
  if (n_written == -1) {
    fprintf(stderr, "failed to sendto(2): %s\n", strerror(errno));
    exit(errno);
  }
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
