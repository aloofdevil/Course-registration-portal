#ifndef UTILS_H
#define UTILS_H

#include <unistd.h>

// send/recv helper
ssize_t send_line(int sockfd, const char *buf);
ssize_t recv_line(int sockfd, char *buf, size_t maxlen);

// file locking wrappers
int lock_file(int fd, int lock_type);
int unlock_file(int fd);

#endif // UTILS_H
