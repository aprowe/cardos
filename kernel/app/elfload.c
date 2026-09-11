/* Loader for CardOS app binaries. See elfload.h. */

#include "kernel/app/elfload.h"
#include "kernel/fs/fs.h"

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

#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP   20

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

typedef struct {
  uint32_t r_offset, r_info;
  int32_t  r_addend;
} Elf32_Rela;

#define ELF32_R_TYPE(i) ((i) & 0xFF)

/* Where capp.ld links each half. Code at 0, data far away, so an absolute
 * value says which half it points into without any symbol lookup. */
#define CAPP_DATA_ORIGIN 0x10000000u

/* An app larger than this is a mistake, not an app: executable RAM is the
 * scarce resource on this board. */
#define CAPP_MAX_CODE (96u * 1024u)
#define CAPP_MAX_DATA (96u * 1024u)

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

CappResult capp_load(const char *path, LoadedApp *out) {
  Elf32_Ehdr eh;
  Elf32_Shdr *sh = NULL;
  uint8_t *code = NULL, *code_w = NULL, *data = NULL;
  uint32_t code_size = 0, data_size = 0;
  int code_sec = -1, data_sec = -1;
  int fd, i;
  CappResult rc = CAPP_ERR_OPEN;

  memset(out, 0, sizeof *out);

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return CAPP_ERR_OPEN;

  if (read_at(fd, 0, &eh, sizeof eh) != 0) goto done;
  if (memcmp(eh.e_ident, "\x7F" "ELF", 4) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  if (eh.e_machine != EM_XTENSA) { rc = CAPP_ERR_WRONG_MACHINE; goto done; }
  if (eh.e_shnum == 0 || eh.e_shnum > 64) { rc = CAPP_ERR_NOT_ELF; goto done; }

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
  if (code_size > CAPP_MAX_CODE || data_size > CAPP_MAX_DATA) {
    rc = CAPP_ERR_TOO_BIG;
    goto done;
  }

  code = heap_caps_malloc(code_size, MALLOC_CAP_EXEC);
  if (!code) {
    ESP_LOGE(TAG, "want %u bytes of exec RAM, %u free (largest block %u)",
             (unsigned)code_size, (unsigned)capp_exec_free(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC));
    rc = CAPP_ERR_NO_MEMORY;
    goto done;
  }
  code_w = writable(code);
  memset(code_w, 0, code_size);

  if (data_size) {
    data = heap_caps_malloc(data_size, MALLOC_CAP_8BIT);
    if (!data) { rc = CAPP_ERR_NO_MEMORY; goto done; }
    memset(data, 0, data_size);
  }

  if (read_at(fd, sh[code_sec].sh_offset, code_w, code_size) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }
  if (data_size && sh[data_sec].sh_type == SHT_PROGBITS &&
      read_at(fd, sh[data_sec].sh_offset, data, data_size) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }

  /* Relocate. Each half was linked at a known origin, so an absolute value
   * says which half it points into and therefore which base to add. Only
   * absolute references move; PC-relative ones are unchanged by shifting a
   * whole section, which is the entire reason this loader is short.
   *
   * Code is patched through its data-window alias, because the compiler is
   * free to implement the read-modify-write with narrower accesses than the
   * instruction window permits. */
  {
    uint32_t code_base = (uint32_t)(uintptr_t)code;
    uint32_t data_base = (uint32_t)(uintptr_t)data;

    for (i = 0; i < eh.e_shnum; i++) {
      Elf32_Rela rela;
      uint32_t n, j;
      int into_code;

      if (sh[i].sh_type != SHT_RELA) continue;
      if ((int)sh[i].sh_info == code_sec) into_code = 1;
      else if ((int)sh[i].sh_info == data_sec) into_code = 0;
      else continue;

      n = sh[i].sh_size / sizeof(Elf32_Rela);
      for (j = 0; j < n; j++) {
        uint32_t type, off, v;
        uint32_t *slot;

        if (read_at(fd, sh[i].sh_offset + j * sizeof rela, &rela, sizeof rela) != 0) {
          rc = CAPP_ERR_RELOC;
          goto done;
        }
        type = ELF32_R_TYPE(rela.r_info);

        if (type == R_XTENSA_NONE || type == R_XTENSA_SLOT0_OP ||
            type == R_XTENSA_ASM_EXPAND) {
          continue;               /* PC-relative, or nothing at all */
        }
        if (type != R_XTENSA_32) {
          ESP_LOGE(TAG, "unsupported relocation type %u", (unsigned)type);
          rc = CAPP_ERR_RELOC;
          goto done;
        }

        /* r_offset is a link-time address, so it carries its section's origin
         * with it. */
        off = into_code ? rela.r_offset : rela.r_offset - CAPP_DATA_ORIGIN;
        if (off + 4 > (into_code ? code_size : data_size)) {
          rc = CAPP_ERR_RELOC;
          goto done;
        }

        slot = (uint32_t *)((into_code ? code_w : data) + off);
        v = *slot;
        *slot = (v >= CAPP_DATA_ORIGIN) ? (v - CAPP_DATA_ORIGIN) + data_base
                                        : v + code_base;
      }
    }
  }

  /* Find the one exported symbol. It is a function, so it lives in the code
   * half by construction, and that half was linked at 0. */
  {
    uint32_t entry = 0;
    int found = 0;

    for (i = 0; i < eh.e_shnum && !found; i++) {
      Elf32_Sym sym;
      char name[32];
      uint32_t n, j, stroff;

      if (sh[i].sh_type != SHT_SYMTAB) continue;
      if (sh[i].sh_link >= eh.e_shnum) continue;
      stroff = sh[sh[i].sh_link].sh_offset;

      n = sh[i].sh_size / sizeof(Elf32_Sym);
      for (j = 0; j < n; j++) {
        if (read_at(fd, sh[i].sh_offset + j * sizeof sym, &sym, sizeof sym) != 0)
          break;
        if (sym.st_name == 0) continue;
        memset(name, 0, sizeof name);
        if (read_at(fd, stroff + sym.st_name, name, sizeof name - 1) != 0) continue;
        if (strcmp(name, "capp_register") == 0) {
          entry = sym.st_value;
          found = 1;
          break;
        }
      }
    }
    if (!found || entry >= code_size) { rc = CAPP_ERR_NO_ENTRY; goto done; }

    {
      const CappApp *(*reg)(const CardApi *);
      extern const CardApi *cardos_api(void);
      const CappApp *app;

      reg = (const CappApp *(*)(const CardApi *))(code + entry);
      app = reg(cardos_api());
      if (!app) { rc = CAPP_ERR_NO_ENTRY; goto done; }
      if (app->api_version != CAPP_API_VERSION) { rc = CAPP_ERR_API; goto done; }

      out->code = code;
      out->data = data;
      out->code_size = code_size;
      out->data_size = data_size;
      out->app = app;
      code = NULL;                 /* handed over */
      data = NULL;
      rc = CAPP_OK;
    }
  }

done:
  fs_close(fd);
  free(sh);
  if (code) heap_caps_free(code);
  if (data) heap_caps_free(data);
  if (rc == CAPP_OK)
    ESP_LOGI(TAG, "loaded %s: %u code at %p, %u data at %p, exec free %u",
             path, (unsigned)out->code_size, out->code,
             (unsigned)out->data_size, out->data, (unsigned)capp_exec_free());
  else
    ESP_LOGW(TAG, "load %s failed: %s", path, capp_strerror(rc));
  return rc;
}

void capp_unload(LoadedApp *la) {
  if (!la) return;
  if (la->code) heap_caps_free(la->code);
  if (la->data) heap_caps_free(la->data);
  memset(la, 0, sizeof *la);
}

const char *capp_strerror(CappResult r) {
  switch (r) {
  case CAPP_OK:                 return "ok";
  case CAPP_ERR_OPEN:           return "cannot open";
  case CAPP_ERR_TOO_BIG:        return "too large";
  case CAPP_ERR_NOT_ELF:        return "not a CardOS app";
  case CAPP_ERR_WRONG_MACHINE:  return "built for another chip";
  case CAPP_ERR_NO_IMAGE:       return "no loadable section";
  case CAPP_ERR_NO_ENTRY:       return "no capp_register";
  case CAPP_ERR_NO_MEMORY:      return "no executable RAM left";
  case CAPP_ERR_RELOC:          return "unsupported relocation";
  case CAPP_ERR_API:            return "built for a different API version";
  }
  return "unknown error";
}
