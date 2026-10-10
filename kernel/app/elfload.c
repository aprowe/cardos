/* Loader for CardOS app binaries. See elfload.h. */

#include "kernel/app/elfload.h"
#include "kernel/app/arena.h"
#include "kernel/app/capprel.h"
#include "kernel/app/xipcache.h"
#include "kernel/app/xipflash.h"
#include "kernel/fs/fs.h"
#include "kernel/sys/applog.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "capp";

/* ---- just enough ELF32 --------------------------------------------------
 * Declared here rather than pulled from a header so the loader stays
 * self-contained and the field offsets are visible where they are used. */

#define EM_XTENSA 94
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2

typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
  uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
  uint32_t st_name, st_value, st_size;
  uint8_t  st_info, st_other;
  uint16_t st_shndx;
} Elf32_Sym;

/* An app larger than this is a mistake, not an app: executable RAM is the
 * scarce resource on this board. */
#define CAPP_MAX_CODE (96u * 1024u)
#define CAPP_MAX_DATA (96u * 1024u)

/* See capp_hold_code. One block: the largest one let go while holding. */
#define SPARE_WANT (16u * 1024u)   /* what a first load asks for when holding */
static void    *s_spare;
static uint32_t s_spare_size;
static int      s_hold;

void capp_hold_code(int on) {
  s_hold = on;
  if (!on && s_spare) {
    heap_caps_free(s_spare);
    s_spare = NULL;
    s_spare_size = 0;
  }
}

/* A code block of at least n bytes: the spare if it fits, else the heap. */
static void *code_alloc(uint32_t n, uint32_t *cap) {
  void *p;
  if (s_spare && s_spare_size >= n) {
    p = s_spare;
    *cap = s_spare_size;
    s_spare = NULL;
    s_spare_size = 0;
    return p;
  }
  /* Holding: ask for room enough for the apps that follow, while there may
   * still be a block that big. Not there is no failure; the exact size is. */
  if (s_hold && n < SPARE_WANT && (p = heap_caps_malloc(SPARE_WANT, MALLOC_CAP_EXEC))) {
    *cap = SPARE_WANT;
    return p;
  }
  p = heap_caps_malloc(n, MALLOC_CAP_EXEC);
  if (!p && s_spare) {                  /* too small to use, big enough to be in the way */
    heap_caps_free(s_spare);
    s_spare = NULL;
    s_spare_size = 0;
    p = heap_caps_malloc(n, MALLOC_CAP_EXEC);
  }
  *cap = n;
  return p;
}

static void code_free(void *p, uint32_t cap) {
  if (!p) return;
  if (s_hold && cap > s_spare_size) {
    if (s_spare) heap_caps_free(s_spare);
    s_spare = p;
    s_spare_size = cap;
    return;
  }
  heap_caps_free(p);
}

/* See capp_last_shortfall. */
static uint32_t s_short_want, s_short_largest;

void capp_last_shortfall(uint32_t *want, uint32_t *largest) {
  if (want) *want = s_short_want;
  if (largest) *largest = s_short_largest;
}

uint32_t capp_exec_free(void) {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_EXEC);
}

/* The byte-addressable view of an executable allocation.
 *
 * heap_caps_malloc(MALLOC_CAP_EXEC) hands back a pointer in the instruction
 * window, where a byte load raises LoadStoreError -- which is exactly how the
 * first version of this crashed, on the memset that cleared the image. The
 * same memory has a data-window alias, and that one is byte-addressable. */
static uint8_t *writable(uint8_t *exec) {
  if (exec && esp_ptr_in_diram_iram(exec))
    return (uint8_t *)esp_ptr_diram_iram_to_dram(exec);
  return exec;
}

static int read_at(int fd, uint32_t off, void *buf, size_t n) {
  if (fs_seek(fd, (int32_t)off, FS_SEEK_SET) < 0) return -1;
  return fs_read(fd, buf, n) == (int)n ? 0 : -1;
}

/* Apply the relocations aimed at section `sec` to the window of it at
 * `win`. Read 32 at a time: one at a time was a seek and a read per entry,
 * and 384 bytes of stack is nothing next to that. */
static CappResult relocate(int fd, const Elf32_Shdr *sh, int shnum, int sec, int into_code,
                           uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                           uint32_t code_base, uint32_t data_base) {
  CappRela buf[32];
  int i;
  for (i = 0; i < shnum; i++) {
    uint32_t n, j, k;
    if (sh[i].sh_type != SHT_RELA || (int)sh[i].sh_info != sec) continue;
    n = sh[i].sh_size / sizeof(CappRela);
    for (j = 0; j < n; j += k) {
      k = n - j < 32 ? n - j : 32;
      if (read_at(fd, sh[i].sh_offset + j * sizeof(CappRela), buf, k * sizeof(CappRela)) != 0)
        return CAPP_ERR_RELOC;
      if (capprel_apply(win, win_off, win_len, limit, into_code, buf, k,
                        code_base, data_base) != 0) {
        ESP_LOGE(TAG, "a relocation in section %d cannot be applied (types: only R_XTENSA_32 is applied)", sec);
        return CAPP_ERR_RELOC;
      }
    }
  }
  return CAPP_OK;
}

/* The two exported symbols, the descriptor and the entry point, found by
 * name in the symbol table. Their values are link addresses; which half each
 * belongs to is decided by where it was linked, as for a relocation. */
static CappResult find_symbols(int fd, const Elf32_Ehdr *eh, const Elf32_Shdr *sh,
                               uint32_t *main_off, uint32_t *info_off) {
  int i, have_main = 0, have_info = 0;

  for (i = 0; i < eh->e_shnum && !(have_main && have_info); i++) {
    Elf32_Sym sym;
    char name[32];
    uint32_t n, j, stroff;

    if (sh[i].sh_type != SHT_SYMTAB) continue;
    if (sh[i].sh_link >= eh->e_shnum) continue;
    stroff = sh[sh[i].sh_link].sh_offset;

    n = sh[i].sh_size / sizeof(Elf32_Sym);
    for (j = 0; j < n; j++) {
      if (read_at(fd, sh[i].sh_offset + j * sizeof sym, &sym, sizeof sym) != 0)
        break;
      if (sym.st_name == 0) continue;
      memset(name, 0, sizeof name);
      if (read_at(fd, stroff + sym.st_name, name, sizeof name - 1) != 0) continue;

      if (!have_main && strcmp(name, "capp_main") == 0) {
        *main_off = sym.st_value;
        have_main = 1;
      } else if (!have_info && strcmp(name, "capp_info") == 0) {
        *info_off = sym.st_value;
        have_info = 1;
      }
    }
  }
  return have_main && have_info ? CAPP_OK : CAPP_ERR_NO_ENTRY;
}

static void (*s_on_prepare)(const char *path);
void capp_on_prepare(void (*fn)(const char *path)) { s_on_prepare = fn; }

/* The code into the flash cache: this file's entry if there is one, else
 * relocated a window at a time through `scratch` -- the arena, which holds
 * nothing yet; this is a launch that is only now claiming it -- and written
 * as a new entry. The whole point is that the code may not fit in one piece
 * of RAM, so it is never asked to. On success the entry is referenced, and
 * capp_unload (or the load's own failure path) lets it go.
 *
 * One lock span from the lookup to the reference: a forget from another
 * task must not program a word into the sectors this is writing, nor kill
 * or let the ring erase the entry between finding it and holding it. */
static CappResult xip_place(int fd, const char *path, const Elf32_Shdr *sh, int shnum,
                            int code_sec, uint32_t code_size, uint8_t *scratch,
                            uint32_t data_base, uint32_t *off_out) {
  XipCache *c = xipflash_cache();
  FsStat st;
  XipKey k;
  uint32_t off, at, n, crc = 0, code_base;
  CappResult rc;

  if (!c || fs_stat(path, &st) != 0) return CAPP_ERR_OPEN;
  memset(&k, 0, sizeof k);
  k.path_hash = xipflash_path_hash(path);
  k.file_size = st.size;
  k.file_mtime = st.mtime;
  k.api = CAPP_API_VERSION;
  k.code_size = code_size;
  k.map_base = xipflash_base();
  k.arena = data_base;
  xipflash_lock();
  if (xip_find(c, &k, &off) == XIP_OK) goto found;

  if (s_on_prepare) s_on_prepare(path);
  if (xip_begin(c, code_size, &off) != XIP_OK) {
    xipflash_unlock();
    applogf("xip", "%s: no room in appcode, code to RAM", path);
    return CAPP_ERR_NO_MEMORY;
  }
  code_base = xipflash_base() + off + XIP_HDR;
  for (at = 0; at < code_size; at += n) {
    n = code_size - at < ARENA_SIZE ? code_size - at : ARENA_SIZE;
    if (read_at(fd, sh[code_sec].sh_offset + at, scratch, n) != 0) { rc = CAPP_ERR_NOT_ELF; goto fail; }
    rc = relocate(fd, sh, shnum, code_sec, 1, scratch, at, n, code_size, code_base, data_base);
    if (rc != CAPP_OK) goto fail;
    crc = xip_crc32(crc, scratch, n);
    if (xip_write(c, at, scratch, n) != XIP_OK) { rc = CAPP_ERR_NO_MEMORY; goto fail; }
  }
  /* Abandoned on failure too: a commit that fails leaves the write pending,
   * and the ring refuses every later begin until it is let go. */
  if (xip_commit(c, &k, crc, path) != XIP_OK) { rc = CAPP_ERR_NO_MEMORY; goto fail; }
  /* Read back before anything runs it. A word that did not program as
   * written would otherwise be found by the next launch's xip_find, after
   * this one had executed it. */
  if (xip_verify(c, off) != XIP_OK) {
    xip_kill(c, off);
    xipflash_unlock();
    applogf("xip", "%s: code at %u did not read back, code to RAM", path, (unsigned)off);
    return CAPP_ERR_NO_MEMORY;
  }
  applogf("xip", "%s: %u bytes of code to flash at %u", path, (unsigned)code_size, (unsigned)off);
found:
  if (xip_ref(c, off) != XIP_OK) {
    xipflash_unlock();
    applogf("xip", "%s: cannot hold the entry at %u, code to RAM", path, (unsigned)off);
    return CAPP_ERR_NO_MEMORY;
  }
  xipflash_unlock();
  *off_out = off;
  return CAPP_OK;
fail:
  xip_abandon(c);
  xipflash_unlock();
  applogf("xip", "%s: cache write failed (%s), code to RAM", path, capp_strerror(rc));
  return rc;
}

/* A reference let go, under the cache's lock like everything else. */
static void xip_release(uint32_t off) {
  xipflash_lock();
  xip_unref(xipflash_cache(), off);
  xipflash_unlock();
}

CappResult capp_load_ex(const char *path, LoadedApp *out, int foreground) {
  Elf32_Ehdr eh;
  Elf32_Shdr *sh = NULL;
  uint8_t *code = NULL, *code_w = NULL, *data = NULL;
  uint32_t code_size = 0, data_size = 0, code_cap = 0;
  uint32_t main_off = 0, info_off = 0, xip_off = 0, code_base, data_base;
  uint16_t head[2];
  int code_sec = -1, data_sec = -1;
  int in_arena = 0, in_flash = 0;
  int fd, i, fsize;
  CappResult rc = CAPP_ERR_OPEN;

  memset(out, 0, sizeof *out);
  out->xip_off = -1;

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return CAPP_ERR_OPEN;

  /* Measured up front so a short file is reported as short. A truncated app
   * otherwise fails wherever it happens to run out -- as "not a CardOS app",
   * which sends you looking at the toolchain instead of at the card. */
  fsize = fs_seek(fd, 0, FS_SEEK_END);

  if (read_at(fd, 0, &eh, sizeof eh) != 0) goto done;
  if (memcmp(eh.e_ident, "\x7F" "ELF", 4) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  if (eh.e_machine != EM_XTENSA) { rc = CAPP_ERR_WRONG_MACHINE; goto done; }
  if (eh.e_shnum == 0 || eh.e_shnum > 64) { rc = CAPP_ERR_NOT_ELF; goto done; }
  if (fsize > 0 &&
      (uint32_t)fsize < eh.e_shoff + (uint32_t)eh.e_shnum * sizeof(Elf32_Shdr)) {
    rc = CAPP_ERR_TRUNCATED;
    goto done;
  }

  sh = (Elf32_Shdr *)malloc((size_t)eh.e_shnum * sizeof *sh);
  if (!sh) { rc = CAPP_ERR_NO_MEMORY; goto done; }
  if (read_at(fd, eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof *sh) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }

  /* Classify the allocatable sections by where they were linked rather than by
   * name: the link address is what the relocations are expressed in. */
  for (i = 0; i < eh.e_shnum; i++) {
    if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
    if (sh[i].sh_type != SHT_PROGBITS && sh[i].sh_type != SHT_NOBITS) continue;
    if (sh[i].sh_size == 0) continue;
    if (sh[i].sh_addr >= CAPP_DATA_ORIGIN) {
      if (data_sec < 0) { data_sec = i; data_size = sh[i].sh_size; }
    } else {
      if (code_sec < 0) { code_sec = i; code_size = sh[i].sh_size; }
    }
  }
  if (code_sec < 0) { rc = CAPP_ERR_NO_IMAGE; goto done; }
  if (fsize > 0 &&
      ((uint32_t)fsize < sh[code_sec].sh_offset + code_size ||
       (data_sec >= 0 && sh[data_sec].sh_type == SHT_PROGBITS &&
        (uint32_t)fsize < sh[data_sec].sh_offset + data_size))) {
    rc = CAPP_ERR_TRUNCATED;
    goto done;
  }
  if (code_size > CAPP_MAX_CODE || data_size > CAPP_MAX_DATA) {
    rc = CAPP_ERR_TOO_BIG;
    goto done;
  }

  /* The symbols before anything is placed: capp_info holds the flags. */
  rc = find_symbols(fd, &eh, sh, &main_off, &info_off);
  if (rc != CAPP_OK) goto done;
  if (main_off >= code_size || data_sec < 0 || sh[data_sec].sh_type != SHT_PROGBITS ||
      info_off < CAPP_DATA_ORIGIN ||
      info_off - CAPP_DATA_ORIGIN + sizeof(CappInfo) > data_size) {
    rc = CAPP_ERR_NO_ENTRY;
    goto done;
  }
  /* Version and flags from the file, before anything is placed: the flags
   * decide where the code goes, and a first flash launch needs the arena
   * empty as scratch, so the data cannot be read first. */
  if (read_at(fd, sh[data_sec].sh_offset + (info_off - CAPP_DATA_ORIGIN), head, sizeof head) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }
  if (head[0] != CAPP_API_VERSION) { rc = CAPP_ERR_API; goto done; }

  /* Data: the arena for the app being started on screen, when it is free
   * and the data fits; the heap otherwise, as before. */
  s_short_want = s_short_largest = 0;
  {
    const char *base = strrchr(path, '/');
    if (foreground &&
        (data = arena_claim(data_size, sh[data_sec].sh_addralign, base ? base + 1 : path)) != NULL)
      in_arena = 1;
  }
  if (!in_arena) {
    data = heap_caps_malloc(data_size, MALLOC_CAP_8BIT);
    if (!data) {
      /* The block that fails after hours of use: one piece of the 8-bit
       * heap, while the total says there is plenty. Said where it can be
       * read later, because nothing else records it. */
      s_short_want = data_size;
      s_short_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      ESP_LOGE(TAG, "want %u bytes of data, largest block %u",
               (unsigned)data_size, (unsigned)s_short_largest);
      applogf("load", "%s: data wants %u, free %u, largest %u", path,
              (unsigned)data_size,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
              (unsigned)s_short_largest);
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
  }
  data_base = (uint32_t)(uintptr_t)data;

  /* Code: from the flash cache when the data is in the arena -- cached code
   * is relocated for one data address, and only the arena keeps one -- and
   * otherwise, or when the cache cannot, in executable RAM as before. RAM
   * code relocated against the arena is as good as any, so a cache failure
   * leaves the data where it is. */
  if (arena_code_in_flash(in_arena, xipflash_ready(), head[1]) &&
      sh[code_sec].sh_addralign <= XIP_HDR &&
      xip_place(fd, path, sh, eh.e_shnum, code_sec, code_size, data, data_base, &xip_off) == CAPP_OK) {
    in_flash = 1;
    code = (uint8_t *)(uintptr_t)(xipflash_base() + xip_off + XIP_HDR);
  } else {
    code = code_alloc(code_size, &code_cap);
    if (!code) {
      s_short_want = code_size;
      s_short_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
      ESP_LOGE(TAG, "want %u bytes of exec RAM, %u free (largest block %u)",
               (unsigned)code_size, (unsigned)capp_exec_free(), (unsigned)s_short_largest);
      applogf("load", "%s: code wants %u, exec free %u, largest %u", path,
              (unsigned)code_size, (unsigned)capp_exec_free(), (unsigned)s_short_largest);
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
    /* Patched through its data-window alias, because the compiler is free
     * to implement the read-modify-write with narrower accesses than the
     * instruction window permits. */
    code_w = writable(code);
    if (read_at(fd, sh[code_sec].sh_offset, code_w, code_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
    rc = relocate(fd, sh, eh.e_shnum, code_sec, 1, code_w, 0, code_size, code_size,
                  (uint32_t)(uintptr_t)code, data_base);
    if (rc != CAPP_OK) goto done;
  }
  code_base = (uint32_t)(uintptr_t)code;

  /* Data last. The arena may hold scratch code from a first launch, so it
   * is cleared first. */
  memset(data, 0, data_size);
  if (read_at(fd, sh[data_sec].sh_offset, data, data_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  rc = relocate(fd, sh, eh.e_shnum, data_sec, 0, data, 0, data_size, data_size, code_base, data_base);
  if (rc != CAPP_OK) goto done;

  out->info = (const CappInfo *)(data + (info_off - CAPP_DATA_ORIGIN));
  out->main = (int (*)(const CardApi *, int, char **))(code + main_off);
  out->code = code;
  out->data = data;
  out->code_size = code_size;
  out->code_cap = code_cap;
  out->data_size = data_size;
  out->xip_off = in_flash ? (int32_t)xip_off : -1;
  out->data_in_arena = (uint8_t)in_arena;
  rc = CAPP_OK;

done:
  fs_close(fd);
  free(sh);
  /* A failure gives back exactly what it took: the cache entry's reference
   * or the code block, the arena or the heap block. */
  if (rc != CAPP_OK) {
    if (in_flash) xip_release(xip_off);
    else code_free(code, code_cap);
    if (in_arena) arena_release(data);
    else if (data) heap_caps_free(data);
  }
  if (rc == CAPP_OK)
    ESP_LOGI(TAG, "loaded %s (%s): %u code (%s), %u data (%s), exec free %u",
             path, out->info->name, (unsigned)out->code_size, in_flash ? "flash" : "RAM",
             (unsigned)out->data_size, in_arena ? "arena" : "heap",
             (unsigned)capp_exec_free());
  else
    ESP_LOGW(TAG, "load %s failed: %s", path, capp_strerror(rc));
  return rc;
}

CappResult capp_load(const char *path, LoadedApp *out) { return capp_load_ex(path, out, 0); }

/* What capp_load_ex took, given back: a reference on the cache entry (the
 * entry itself stays, for the next launch) or the code block, and the arena
 * or the heap block. */
void capp_unload(LoadedApp *la) {
  if (!la) return;
  if (la->xip_off >= 0) xip_release((uint32_t)la->xip_off);
  else code_free(la->code, la->code_cap);
  if (la->data_in_arena) arena_release(la->data);
  else if (la->data) heap_caps_free(la->data);
  memset(la, 0, sizeof *la);
  la->xip_off = -1;
}

const char *capp_strerror(CappResult r) {
  switch (r) {
  case CAPP_OK:                 return "ok";
  case CAPP_ERR_OPEN:           return "cannot open";
  case CAPP_ERR_TOO_BIG:        return "too large";
  case CAPP_ERR_NOT_ELF:        return "not a CardOS app";
  case CAPP_ERR_TRUNCATED:      return "truncated file";
  case CAPP_ERR_WRONG_MACHINE:  return "built for another chip";
  case CAPP_ERR_NO_IMAGE:       return "no loadable section";
  case CAPP_ERR_NO_ENTRY:       return "no capp_register";
  case CAPP_ERR_NO_MEMORY:      return "no executable RAM left";
  case CAPP_ERR_RELOC:          return "unsupported relocation";
  case CAPP_ERR_API:            return "built for a different API version";
  }
  return "unknown error";
}
