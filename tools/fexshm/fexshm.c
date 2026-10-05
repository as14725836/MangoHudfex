#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * fexshm - redirect POSIX shm_open() to a regular file in a writable dir.
 *
 * FEX-Emu publishes its stats through shm_open("/fex-<pid>-stats"), which can
 * only land in the glibc SHMDIR (/dev/shm). Native Termux has no /dev/shm, so
 * the file is never created and overlays report "FEX Not Found!".
 *
 * With this library preloaded, the file lands in $FEXSHM_DIR (default below),
 * which MangoHud already scans. Only flat names are intercepted; anything else
 * falls back to the plain /dev/shm path, exactly like glibc would.
 *
 * No dlsym() on purpose: keeps the dependency at GLIBC_2.17 so old Termux
 * glibc builds can load it.
 *
 * Build:  gcc -shared -fPIC -O2 -o libfexshm.so fexshm.c
 */

#ifndef FEXSHM_DEFAULT_DIR
#define FEXSHM_DEFAULT_DIR "/data/data/com.termux/files/usr/tmp"
#endif

static const char* shm_dir(void) {
  const char* d = getenv("FEXSHM_DIR");
  return (d && *d) ? d : FEXSHM_DEFAULT_DIR;
}

static int flat_name(const char* name) {
  if (!name || !*name) return 0;
  if (*name == '/') ++name;
  return *name && !strchr(name, '/');
}

int shm_open(const char* name, int oflag, ...) {
  char path[512];
  mode_t mode = 0;
  va_list ap;

  if (oflag & O_CREAT) {
    va_start(ap, oflag);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  if (!flat_name(name)) {
    /* Unusual name: behave like glibc (plain /dev/shm). */
    if (!name || name[0] != '/') { errno = EINVAL; return -1; }
    if ((size_t)snprintf(path, sizeof path, "/dev/shm%s", name) >= sizeof path) {
      errno = ENAMETOOLONG;
      return -1;
    }
    return open(path, oflag | O_CLOEXEC, mode);
  }

  if (name[0] == '/') ++name;
  if ((size_t)snprintf(path, sizeof path, "%s/%s", shm_dir(), name) >= sizeof path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (oflag & O_CREAT) mkdir(shm_dir(), 0777);
  return open(path, oflag | O_CLOEXEC, mode);
}

int shm_open64(const char* name, int oflag, ...) {
  mode_t mode = 0;
  va_list ap;
  if (oflag & O_CREAT) {
    va_start(ap, oflag);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return shm_open(name, oflag, mode);
}

int shm_unlink(const char* name) {
  char path[512];
  if (!flat_name(name)) {
    if (!name || name[0] != '/') { errno = EINVAL; return -1; }
    if ((size_t)snprintf(path, sizeof path, "/dev/shm%s", name) >= sizeof path) {
      errno = ENAMETOOLONG;
      return -1;
    }
    return unlink(path);
  }
  if (name[0] == '/') ++name;
  if ((size_t)snprintf(path, sizeof path, "%s/%s", shm_dir(), name) >= sizeof path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return unlink(path);
}
