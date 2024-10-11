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
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>

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
  uint8_t end;
} Arena;

typedef struct {
  uint64_t buf_idx;
  //  DynArrayU8 buf;
  int socket;
} LineBufferedReader;

void *arena_alloc(Arena *a, uint64_t size, uint64_t align, uint64_t count) {
  const uint64_t padding = -(uint64_t)a->start & (align - 1);
  const uint64_t available = (uint64_t)a->end - (uint64_t)a->start - padding;
  if (available < 0 || count > available / size) {
    abort();
  }

  void *res = a->start + padding;
  a->start += padding + count * size;

  return memset(res, 0, count * size);
}
#define arena_new(a, t, n) (t *)arena_alloc(a, sizeof(t), _Alignof(t), n)

static HttpRequest request_read() {
  HttpRequest res = {0};
  return res;
}

static void handle_connection(int conn_fd) {
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
