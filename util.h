/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in
 *  the LICENSE file in the root directory of this source tree. An
 *  additional grant of patent rights can be found in the PATENTS file
 *  in the same directory.
 *
 */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include "dbg.h"

#ifndef ECOMM
#define ECOMM EBADRPC
#endif

#define ARRAYSIZE(ar) (sizeof (ar) / sizeof (*(ar)))

// Suppress compiler warning about unused computation
static inline bool verify_dummy(bool x) { return x; }

#ifdef NDEBUG
# define VERIFY(x) (verify_dummy(x))
#else
# define VERIFY(x) (assert(x))
#endif

// Reslists own resources (or more precisely, they contain lists of
// cleanup closures).  Every time we allocate a resource, we mark it
// owned by the reslist named by _reslist_current.  reslist deallocate
// their owned resources when they are destroyed.  Scoped reslists are
// automatically destroyed when they go out of scope.  They go out of
// scope either on normal return (in which case the compiler runs the
// cleanup) or on die(), in which case the reslist machiney takes care
// of running the cleanups.
//
// The only operations that can affect the _reslist_current are
// SCOPED_RESLIST and WITH_CURRENT_RESLIST.

typedef void (*cleanupfn)(void* data);

struct resource {
    enum { RES_RESLIST_ONHEAP, RES_RESLIST_ONSTACK, RES_CLEANUP } type;
    // Circular list
    struct resource* prev;
    struct resource* next;
};

struct reslist {
    struct resource r;
    struct reslist* parent;
    struct resource head;
};

struct cleanup {
    struct resource r;
    cleanupfn fn;
    void* fndata;
};

// Create a new reslist owned by _reslist_current.
// Does _not_ make the new reslist current.
struct reslist* reslist_create(void);

// Destroy a reslist.  Cleans up all resources owned by that reslist.
void reslist_destroy(struct reslist* rl);

// Transfer resources owned by resist DONOR to reslist RECIPIENT.
// DONOR's resources are spliced in-order to the head of RECIPIENT.
// That is, when RECIPIENT is destroyed, all of DONOR's resources are
// cleaned up, and then all of RECIPIENT's.
void reslist_xfer(struct reslist* recipient, struct reslist* donor);

void _reslist_scoped_push(struct reslist* rl);
void _reslist_scoped_pop(struct reslist* rl);

void _reslist_guard_push(struct reslist** saved_rl, struct reslist* rl);
void _reslist_guard_pop(struct reslist** saved_rl);

#define PASTE(a,b) a##b
#define GENSYM(sym) PASTE(sym, __LINE__)

#define SCOPED_RESLIST(varname)                                 \
    __attribute__((cleanup(_reslist_scoped_pop)))               \
    struct reslist PASTE(varname,_reslist_);                    \
    struct reslist* varname = &PASTE(varname,_reslist_);        \
    _reslist_scoped_push(varname)

#define WITH_CURRENT_RESLIST(_rl)                               \
    __attribute__((cleanup(_reslist_guard_pop)))                \
    struct reslist* GENSYM(_reslist_saved);                     \
    _reslist_guard_push(&GENSYM(_reslist_saved), (_rl))

// Each resource owned by a reslist is associated with a cleanup
// function.  Adding a cleanup function requires allocating memory and
// can fail.  To make sure we can clean up every resource we allocate,
// we allocate a cleanup *before* the resource it owns, allocate the
// resource, and commit the cleanup object to that resource.
// The commit operation cannot fail.

// Allocate a new cleanup object. The storage for the cleanup object
// itself is owned by the current reslist (and is heap-allocated, and
// so can fail), but the new cleanup owns no resource.
struct cleanup* cleanup_allocate(void);

// Commit the cleanup object to a resource.  The cleanup object must
// have been previously allocated with cleanup_allocate.  A given
// cleanup object can be committed to a resource once.  When a cleanup
// object is allocated to a resource, it is re-inserted at the head of
// the current reslist.  Reslists clean up their resources in reverse
// order of insertion.
void cleanup_commit(struct cleanup* cl, cleanupfn fn, void* fndata);

// Deregister and deallocate the given cleanup object CL, but do not
// run any cleanup functions to which CL may have been committed.
// If CL is NULL, do nothing.
void cleanup_forget(struct cleanup* cl);

// Commit a cleanup object to closing the given file descriptor.
void cleanup_commit_close_fd(struct cleanup* cl, int fd);

// Allocate and commit a cleanup object that will unlink a file of the
// given name.  Failure to unlink the file is ignored.
struct unlink_cleanup;
struct unlink_cleanup* unlink_cleanup_allocate(const char* filename);
void unlink_cleanup_commit(struct unlink_cleanup* ucl);

// Allocate memory owned by the current reslist.
__attribute__((malloc))
void* xalloc(size_t sz);
__attribute__((malloc))
void* xcalloc(size_t sz);

// Open a file.  The returned file descriptor is owned by the
// current reslist.
int xopen(const char* pathname, int flags, mode_t mode);

// Close a file descriptor.  Fail if FD is not an open file
// descriptor.  Do not use to close file descriptors owned
// by reslists.
void xclose(int fd);

// Allocate a pipe.  The file descriptors are owned by the
// current reslits.
void xpipe(int* read_end, int* write_end);

// Duplicate a file descriptor.  The new file descriptor is owned by
// the current reslist.
int xdup(int fd);
int xdup3nc(int oldfd, int newfd, int flags);

// Open a file descriptor as a stream.  The returned FILE object is
// owned by the current reslist.  It does _not_ own FD.  Instead, FILE
// owns a new, duped file descriptor.
FILE* xfdopen(int fd, const char* mode);

// All file descriptors we allocate are configured to be
// close-on-exit.  This routine allows FD to be inherited across exec.
void allow_inherit(int fd);

// Code that fails calls die() or one of its variants below.
// Control then flows to the nearest enclosing catch_error.

typedef struct errinfo {
    int err;
    const char* msg;
    const char* prgname;
    unsigned want_msg : 1;
} errinfo;

// Call FN with FNDATA with an internal resource list as current.
// If FN returns normally, transfer resources added to that resource
// list to the resource list that was current at the time of
// catch_error.  On error, destroy the resource list.  Return true on
// normal return or false on error.  If EI is non-null, fill it on
// error.  Strings are allocated on the resource list in effect at the
// time catch_error is called.  If want_msg is zero, error strings are
// not allocated, but ei->err is still set.
bool catch_error(void (*fn)(void* fndata),
                 void* fndata,
                 struct errinfo* ei);

__attribute__((noreturn))
void diev(int err, const char* fmt, va_list args);
__attribute__((noreturn,format(printf, 2, 3)))
void die(int err, const char* fmt, ...);
__attribute__((noreturn,format(printf, 1, 2)))
void die_errno(const char* fmt, ...);
__attribute__((noreturn))
void die_oom(void);

// A FDH (File Descriptor Handle) is a package of a file descriptor
// and a reslist that owns it.  It allows us to allocate an file
// descriptor owned by a reslist and close that file descriptor before
// its owning reslist is destroyed.

struct fdh {
    struct reslist* rl; // Owns both fd and fdh
    int fd;
};

// Duplicate an existing FD as an FDH
struct fdh* fdh_dup(int fd);

// Deallocate an FDH, closing its file descriptor.  FDH is invalid
// after this call.
void fdh_destroy(struct fdh* fdh);

__attribute__((malloc,format(printf, 1, 2)))
char* xaprintf(const char* fmt, ...);
__attribute__((malloc))
char* xavprintf(const char* fmt, va_list args);
__attribute__((malloc))
char* xstrdup(const char* s);
char* xstrndup(const char* s, size_t nr);

bool error_temporary_p(int errnum);

extern const char* orig_argv0;
extern const char* prgname;
void set_prgname(const char* s);
extern int real_main(int argc, char** argv);

size_t nextpow2sz(size_t sz);

#define XMIN(a,b)                \
    ({ typeof (a) _a = (a);      \
        typeof (a) _b = (b);     \
        _a > _b ? _b : _a; })

#define XMAX(a,b)                \
    ({ typeof (a) _a = (a);      \
        typeof (a) _b = (b);     \
        _a > _b ? _a : _b; })

#define SATADD(r, a, b)                         \
    ({                                          \
        typedef __typeof((*(r))) xT;            \
        xT xa = (a);                            \
        xT xb = (b);                            \
        int overflow;                           \
        if (((xT) -1) - xa < xb) {              \
            overflow = 1;                       \
            *(r) = ((xT) -1);                   \
        } else {                                \
            overflow = 0;                       \
            *(r) = xa + xb;                     \
        }                                       \
                                                \
        overflow;                               \
    })

#define XPOW2P(v)                               \
    ({                                          \
        __typeof((v)) _v = (v);                 \
        (_v & (_v-1)) == 0;                     \
    })

struct iovec;
size_t iovec_sum(const struct iovec* iov, unsigned niovec);

enum blocking_mode { blocking, non_blocking };
enum blocking_mode fd_set_blocking_mode(int fd, enum blocking_mode mode);

void hack_reopen_tty(int fd);

// Read SZ bytes from FD into BUF, retrying on EINTR.
// May return short read on EOF.
size_t read_all(int fd, void* buf, size_t sz);

// Write SZ bytes to FD, retrying on EINTR.
void write_all(int fd, const void* buf, size_t sz);

#ifndef HAVE_DUP3
int dup3(int oldfd, int newfd, int flags);
#endif

#define XPPOLL_LINUX_SYSCALL 1
#define XPPOLL_KQUEUE 2
#define XPPOLL_SYSTEM 3
#define XPPOLL_STUPID_WRAPPER 4

#if defined(__linux__)
# define XPPOLL XPPOLL_LINUX_SYSCALL
#elif defined(HAVE_KQUEUE)
# define XPPOLL XPPOLL_KQUEUE
#elif defined(HAVE_PPOLL)
# define XPPOLL XPPOLL_SYSTEM
# define XPPOLL_BROKEN 1
#else
# define XPPOLL XPPOLL_STUPID_WRAPPER
# define XPPOLL_BROKEN 1
#endif

// See ppoll(2) or ppoll(3).  If XPPOLL_BROKEN is defined, this
// routine may not atomically set SIGMASK and begin waiting, so you'll
// need to arrange for some other wakeup mechanism to avoid racing
// against signal delivery.
int xppoll(struct pollfd *fds, nfds_t nfds,
           const struct timespec *timeout_ts,
           const sigset_t *sigmask);

#ifndef HAVE_MKOSTEMP
int mkostemp(char *template, int flags);
#endif

FILE* xnamed_tempfile(const char** name);

#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE 0
#endif

void replace_with_dev_null(int fd);
void* generate_random_bytes(size_t howmany);
char* hex_encode_bytes(const void* bytes, size_t n);
char* gen_hex_random(size_t nr_bytes);

void* first_non_null(void* s, ...);
bool string_starts_with_p(const char* string, const char* prefix);

#ifdef HAVE_CLOCK_GETTIME
double xclock_gettime(clockid_t clk_id);
#endif

#ifndef NDEBUG
void assert_cloexec(int fd);
#else
# define assert_cloexec(_fd) ((void)(_fd))
#endif

int merge_O_CLOEXEC_into_fd_flags(int fd, int flags);

extern sigset_t signals_unblock_for_io;
extern sigset_t orig_sigmask;
extern int signal_quit_in_progress;

void _unblock_io_unblocked_signals(sigset_t* saved);
void _restore_io_unblocked_signals(sigset_t* saved);

#define WITH_IO_SIGNALS_ALLOWED()                                       \
    __attribute__((cleanup(_restore_io_unblocked_signals)))             \
    sigset_t GENSYM(_saved_signals);                                    \
    _unblock_io_unblocked_signals(&GENSYM(_saved_signals))

void save_signals_unblock_for_io(void);
void sigaction_restore_as_cleanup(int signo, struct sigaction* sa);

// Like execvpe, but actually works on bionic.  Die on error.
// Not async-signal-safe.
__attribute__((noreturn))
void xexecvpe(const char* file,
              const char* const* argv,
              const char* const* envp);
