#ifndef NOTCURSES_FBUF
#define NOTCURSES_FBUF

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "compat/compat.h"

// a growable buffer into which one can perform formatted i/o, like the
// ten thousand that came before it, and the ten trillion which shall
// come after. uses mmap (with huge pages, if possible) on unix and
// virtualalloc on windows. it can grow arbitrarily large. it does
// *not* maintain a NUL terminator, and can hold binary data.

typedef struct fbuf {
  uint64_t size;
  uint64_t used;
#ifdef __MINGW64__
  LPVOID buf;
#else
  char* buf;
#endif
} fbuf;

// header-only so that we can test it from notcurses-tester

static inline char*
fbuf_at(fbuf* f, uint64_t at){
  if(at > f->used){
    return NULL;
  }
  return f->buf + at;
}

#ifdef MAP_POPULATE
#ifdef MAP_UNINITIALIZED
#define MAPFLAGS (MAP_POPULATE | MAP_UNINITIALIZED)
#else
#define MAPFLAGS MAP_POPULATE
#endif
#else
#ifdef MAP_UNINITIALIZED
#define MAPFLAGS MAP_UNINITIALIZED
#else
#define MAPFLAGS 0
#endif
#endif

// ensure there is sufficient room to add |n| bytes to |f|. if necessary,
// enlarge the buffer, which might move it (invalidating any references
// therein). the amount added is based on the current size (and |n|). we
// never grow larger than SIZE_MAX / 2.
static inline int
fbuf_grow(fbuf* f, size_t n){
  assert(NULL != f->buf);
  assert(0 != f->size);
  size_t size = f->size;
  if(size - f->used >= n){
    return 0; // we have enough space
  }
  while(SIZE_MAX / 2 >= size){
    size *= 2;
    if(size - f->used < n){
      continue;
    }
    void* tmp;
#ifdef __linux__
    tmp = mremap(f->buf, f->size, size, MREMAP_MAYMOVE);
    if(tmp == MAP_FAILED){
      return -1;
    }
#else
    tmp = realloc(f->buf, f->size);
    if(tmp == NULL){
      return -1;
    }
#endif
    f->buf = (char*)tmp; // cast for c++ callers
    f->size = size;
    return 0;
  }
  // n (or our current buffer) is too large
  return -1;
}

// prepare (a significant amount of) initial space for the fbuf.
// pass 1 for |small| if it ought be...small.
static inline int
fbuf_initgrow(fbuf* f, unsigned small){
  assert(NULL == f->buf);
  assert(0 == f->used);
  assert(0 == f->size);
  // we start with 2MiB, the huge page size on all of x86+PAE,
  // ARMv7+LPAE, ARMv8, and x86-64.
  // FIXME use GetLargePageMinimum() and sysconf
  size_t size = small ? (4096 > BUFSIZ ? 4096 : BUFSIZ) : 0x200000lu;
#if defined(__linux__)
  static bool hugepages_failed = false; // FIXME atomic
  if(!hugepages_failed && !small){
    // mmap(2): hugetlb results in automatic stretch out to cover hugepage
    f->buf = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_HUGETLB |
                         MAP_PRIVATE | MAP_ANONYMOUS | MAPFLAGS , -1, 0);
    if(f->buf == MAP_FAILED){
      hugepages_failed = true;
      f->buf = NULL;
    }
  }
  if(f->buf == NULL){ // try again without MAP_HUGETLB
    f->buf = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAPFLAGS , -1, 0);
  }
  if(f->buf == MAP_FAILED){
    f->buf = NULL;
    return -1;
  }
#else
  f->buf = (char*)malloc(size);
  if(f->buf == NULL){
    return -1;
  }
#endif
  f->size = size;
  return 0;
}
#undef MAPFLAGS

// prepare f with a small initial buffer.
static inline int
fbuf_init_small(fbuf* f){
  f->used = 0;
  f->size = 0;
  f->buf = NULL;
  return fbuf_initgrow(f, 1);
}

// prepare f with an initial buffer.
static inline int
fbuf_init(fbuf* f){
  f->used = 0;
  f->size = 0;
  f->buf = NULL;
  return fbuf_initgrow(f, 0);
}

// reset usage, but don't shrink the buffer or anything
static inline void
fbuf_reset(fbuf* f){
  f->used = 0;
}

static inline int
fbuf_putc(fbuf* f, char c){
  if(fbuf_grow(f, 1)){
    return -1;
  }
  *fbuf_at(f, f->used++) = c;
  return 1;
}

static inline int
fbuf_putn(fbuf* f, const char* s, size_t len){
  if(fbuf_grow(f, len)){
    return -1;
  }
  memcpy(f->buf + f->used, s, len);
  f->used += len;
  return len;
}

static inline int
fbuf_puts(fbuf* f, const char* s){
  size_t slen = strlen(s);
  return fbuf_putn(f, s, slen);
}

static inline int
fbuf_putint(fbuf* f, int n){
  if(fbuf_grow(f, 10)){ // 32-bit int might require up to 10 digits
    return -1;
  }
  uint64_t r = snprintf(f->buf + f->used, f->size - f->used, "%d", n);
  if(r > f->size - f->used){
    assert(r <= f->size - f->used);
    return -1; // FIXME grow?
  }
  f->used += r;
  return r;
}

// FIXME eliminate this, ideally
__attribute__ ((format (printf, 2, 3)))
static inline int
fbuf_printf(fbuf* f, const char* fmt, ...){
  if(fbuf_grow(f, BUFSIZ) < 0){
    return -1;
  }
  va_list va;
  va_start(va, fmt);
  int r = vsnprintf(f->buf + f->used, f->size - f->used, fmt, va);
  va_end(va);
  if((size_t)r >= f->size - f->used){
    return -1;
  }
  assert(r >= 0);
  f->used += r;
  return r;
}

// emit an escape; obviously you can't flush here
static inline int
fbuf_emit(fbuf* f, const char* esc){
  if(!esc){
    return -1;
  }
  if(fbuf_puts(f, esc) < 0){
    //logerror("error emitting escape (%s)\n", strerror(errno));
    return -1;
  }
  return 0;
}

// releases the resources held by f. f itself is not freed.
static inline void
fbuf_free(fbuf* f){
  if(f){
//    logdebug("Releasing from %" PRIu32 "B (%" PRIu32 "B)\n", f->size, f->used);
    if(f->buf){
#if __linux__
      if(munmap(f->buf, f->size)){
        //logwarn("Error unmapping alloc (%s)\n", strerror(errno));
      }
#else
      free(f->buf);
#endif
      f->buf = NULL;
    }
    f->size = 0;
    f->used = 0;
  }
}

// attempt to write the contents of |f| to the FILE |fp|, if there are any
// contents, and free the fbuf either way. if |flushfp| is set, fflush(fp).
static inline int
fbuf_finalize(fbuf* f, FILE* fp, bool flushfp){
  int ret = 0;
  if(f->used){
    if(fwrite(f->buf, f->used, 1, fp) != 1){
      ret = -1;
    }
  }
  if(flushfp && ret == 0 && fflush(fp) == EOF){
    ret = -1;
  }
  fbuf_free(f);
  return ret;
}

#ifdef __cplusplus
}
#endif

#endif