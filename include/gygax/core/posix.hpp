#pragma once

#include <sys/socket.h>

#include <fcntl.h>

#include <cerrno>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// "Remote I/O error": the device answered, but with an error (a Modbus exception, an OBD-II
// negative response). Linux has it; macOS doesn't, so use Linux's value, which macOS leaves
// unassigned and therefore never collides with a real errno there.
// socket(..., SOCK_CLOEXEC) is Linux/BSD; macOS sets close-on-exec afterwards (openSocket).
#ifdef SOCK_CLOEXEC
#define GYGAX_SOCK_CLOEXEC SOCK_CLOEXEC
#else
#define GYGAX_SOCK_CLOEXEC 0
#endif

#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

namespace gygax {

// socket() with close-on-exec set on every platform.
inline int openSocket(int family, int type, int protocol) {
    const int fd = ::socket(family, type | GYGAX_SOCK_CLOEXEC, protocol);
#ifndef SOCK_CLOEXEC
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    return fd;
}

inline void suppressSigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

} // namespace gygax
