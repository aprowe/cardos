/* The Python app's half of MicroPython's port: files on the card, imports
 * from it, a small os and time, sys.stdout, and exit() (2026-10-16).
 *
 * Code, not declarations, but a .h: python.c includes it once, after the
 * stdout hooks, so `api` and the log are in scope (and build_apps.py, which
 * takes every .c in an app's folder for an app, leaves it alone). It is a
 * file of its own because MicroPython's
 * generator scans it for the names it uses (MP_QSTR_...) and the modules it
 * registers (tools/make_micropython.sh): run that again after adding one.
 * So it includes MicroPython's headers only -- the generator has no
 * kernel/app/capp.h -- and the scan sees `api->...` as plain words.
 *
 * Paths: anything not starting with / is against the current directory,
 * which is the running script's folder (os.chdir moves it), "/" otherwise.
 * sys.path is ['', '/lib']: a module is found beside the script, then in
 * /lib on the card.
 *
 * Files: the card has eight descriptors for the whole OS, and an app is not
 * told when fn-` closes it, so Python holds at most FILES_MAX at once, a
 * file nothing refers to is closed when the heap is collected (a
 * finaliser), a collection is tried before refusing a fifth, every run of a
 * script ends with one, and leaving by exit() closes them all. One still
 * open when fn-` takes the app away is lost until a restart.
 */
#include "py/builtin.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "py/reader.h"
#include "py/runtime.h"
#include "py/smallint.h"
#include "py/stream.h"

#define PATH_MAX_  160
#define FILES_MAX  4
#define LIST_MAX   64

static char cwd[PATH_MAX_] = "/";
static int files[FILES_MAX] = { -1, -1, -1, -1 };

/* ---- paths -------------------------------------------------------------------- */

/* p made absolute against cwd, with . and .. and doubled slashes gone. */
static int norm_path(const char *p, char *out, size_t n) {
  size_t len = 0;
  if (p[0] != '/') {
    len = strlen(cwd);
    if (len + 1 > n) return -1;
    memcpy(out, cwd, len);
    if (len == 1) len = 0;                       /* "/" is the empty prefix */
  }
  while (*p) {
    const char *e;
    size_t k;
    while (*p == '/') p++;
    if (!*p) break;
    for (e = p; *e && *e != '/'; e++) {
    }
    k = (size_t)(e - p);
    if (k == 2 && p[0] == '.' && p[1] == '.') {
      while (len && out[len - 1] != '/') len--;
      if (len) len--;
    } else if (!(k == 1 && p[0] == '.')) {
      if (len + 1 + k + 1 > n) return -1;
      out[len++] = '/';
      memcpy(out + len, p, k);
      len += k;
    }
    p = e;
  }
  if (!len) out[len++] = '/';
  out[len] = 0;
  return 0;
}

static const char *path_arg(mp_obj_t o, char *out) {
  const char *p = mp_obj_str_get_str(o);
  if (norm_path(p, out, PATH_MAX_)) mp_raise_OSError(MP_EINVAL);
  return out;
}

/* cwd becomes the folder `path` (absolute, normalised) is in. */
static void cwd_to_folder_of(const char *path) {
  size_t k = strlen(path);
  while (k && path[k - 1] != '/') k--;
  if (k > 1) k--;                                 /* keep "/" for the root */
  if (k >= sizeof cwd) return;
  memcpy(cwd, path, k);
  cwd[k] = 0;
}

/* ---- imports ------------------------------------------------------------------ */

mp_import_stat_t mp_import_stat(const char *path) {
  char p[PATH_MAX_];
  CappStat st;
  if (norm_path(path, p, sizeof p) || api->stat(p, &st)) return MP_IMPORT_STAT_NO_EXIST;
  return st.is_dir ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

typedef struct {
  int fd;
  uint16_t n, at;
  byte buf[128];
} cp_reader_t;

static mp_uint_t reader_byte(void *data) {
  cp_reader_t *r = data;
  if (r->at >= r->n) {
    int k = r->fd < 0 ? 0 : api->read(r->fd, r->buf, sizeof r->buf);
    if (k <= 0) return MP_READER_EOF;
    r->n = (uint16_t)k;
    r->at = 0;
  }
  return r->buf[r->at++];
}

static void reader_close(void *data) {
  cp_reader_t *r = data;
  if (r->fd >= 0) api->close(r->fd);
  r->fd = -1;
  m_del_obj(cp_reader_t, r);
}

/* The source of a module or a script, a block of the card at a time. */
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
  char p[PATH_MAX_];
  mp_reader_t reader;
  cp_reader_t *r = m_new_obj(cp_reader_t);       /* before the open: it may raise */
  r->n = r->at = 0;
  r->fd = norm_path(qstr_str(filename), p, sizeof p) ? -1 : api->open(p, CAPP_O_READ);
  if (r->fd < 0) {
    m_del_obj(cp_reader_t, r);
    mp_raise_OSError_with_filename(MP_ENOENT, qstr_str(filename));
  }
  reader.data = r;
  reader.readbyte = reader_byte;
  reader.close = reader_close;
  return mp_lexer_new(filename, reader);
}

/* ---- files -------------------------------------------------------------------- */

typedef struct {
  mp_obj_base_t base;
  int fd;
} cp_file_t;

static void file_forget(int fd) {
  int i;
  for (i = 0; i < FILES_MAX; i++)
    if (files[i] == fd) files[i] = -1;
}

static void files_close_all(void) {
  int i;
  for (i = 0; i < FILES_MAX; i++)
    if (files[i] >= 0) {
      api->close(files[i]);
      files[i] = -1;
    }
}

static int file_slot(void) {
  int i;
  for (i = 0; i < FILES_MAX; i++)
    if (files[i] < 0) return i;
  return -1;
}

static mp_uint_t file_read(mp_obj_t o, void *buf, mp_uint_t n, int *err) {
  cp_file_t *f = MP_OBJ_TO_PTR(o);
  int k;
  if (f->fd < 0) {
    *err = MP_EBADF;
    return MP_STREAM_ERROR;
  }
  k = api->read(f->fd, buf, n);
  if (k < 0) {
    *err = MP_EIO;
    return MP_STREAM_ERROR;
  }
  return (mp_uint_t)k;
}

static mp_uint_t file_write(mp_obj_t o, const void *buf, mp_uint_t n, int *err) {
  cp_file_t *f = MP_OBJ_TO_PTR(o);
  int k;
  if (f->fd < 0) {
    *err = MP_EBADF;
    return MP_STREAM_ERROR;
  }
  k = api->write(f->fd, buf, n);
  if (k < 0) {
    *err = MP_EIO;
    return MP_STREAM_ERROR;
  }
  return (mp_uint_t)k;
}

static mp_uint_t file_ioctl(mp_obj_t o, mp_uint_t req, uintptr_t arg, int *err) {
  cp_file_t *f = MP_OBJ_TO_PTR(o);
  if (req == MP_STREAM_CLOSE) {
    if (f->fd >= 0) {
      api->close(f->fd);
      file_forget(f->fd);
      f->fd = -1;
    }
    return 0;
  }
  if (f->fd < 0) {
    *err = MP_EBADF;
    return MP_STREAM_ERROR;
  }
  if (req == MP_STREAM_SEEK) {
    struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
    int at = api->seek(f->fd, (int32_t)s->offset, s->whence);
    if (at < 0) {
      *err = MP_EINVAL;
      return MP_STREAM_ERROR;
    }
    s->offset = at;
    return 0;
  }
  if (req == MP_STREAM_FLUSH) return 0;
  *err = MP_EINVAL;
  return MP_STREAM_ERROR;
}

static void file_print(const mp_print_t *print, mp_obj_t o, mp_print_kind_t kind) {
  (void)kind;
  mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(o), ((cp_file_t *)MP_OBJ_TO_PTR(o))->fd);
}

static const mp_rom_map_elem_t file_locals_table[] = {
  { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
  { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
  { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
  { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
  { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
  { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
  { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
  { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
  { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
  { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
  { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
  { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals, file_locals_table);

static const mp_stream_p_t file_stream = { .read = file_read, .write = file_write,
                                           .ioctl = file_ioctl, .is_text = false };
static const mp_stream_p_t text_stream = { .read = file_read, .write = file_write,
                                           .ioctl = file_ioctl, .is_text = true };

static MP_DEFINE_CONST_OBJ_TYPE(cp_fileio_type, MP_QSTR_FileIO, MP_TYPE_FLAG_ITER_IS_STREAM,
                                print, file_print, protocol, &file_stream,
                                locals_dict, &file_locals);
static MP_DEFINE_CONST_OBJ_TYPE(cp_textio_type, MP_QSTR_TextIOWrapper,
                                MP_TYPE_FLAG_ITER_IS_STREAM, print, file_print,
                                protocol, &text_stream, locals_dict, &file_locals);

/* open(path, mode='r'): r w a, + for both ways, b for bytes. The card's
 * open has no append that the fake card shares, so `a` opens to write
 * without truncating (making the file if it is not there) and seeks to the
 * end. Other arguments (encoding, ...) are accepted and ignored. */
mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
  static const mp_arg_t allowed[] = {
    { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_r) } },
    { MP_QSTR_buffering, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_encoding, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
  };
  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  char p[PATH_MAX_];
  const char *m;
  int flags = 0, plus = 0, bin = 0, append = 0, fd, slot;
  CappStat st;
  cp_file_t *f;
  mp_arg_parse_all(n_args, args, kw, MP_ARRAY_SIZE(allowed), allowed, a);
  path_arg(a[0].u_obj, p);
  for (m = mp_obj_str_get_str(a[1].u_obj); *m; m++) {
    switch (*m) {
      case 'r': flags = CAPP_O_READ; break;
      case 'w': flags = CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC; break;
      case 'a': flags = CAPP_O_WRITE; append = 1; break;
      case '+': plus = 1; break;
      case 'b': bin = 1; break;
      case 't': break;
      default: mp_raise_ValueError(MP_ERROR_TEXT("mode"));
    }
  }
  if (!flags) mp_raise_ValueError(MP_ERROR_TEXT("mode"));
  if (plus) flags |= CAPP_O_READ | CAPP_O_WRITE;
  if (!api->stat(p, &st) && st.is_dir) mp_raise_OSError_with_filename(MP_EISDIR, p);
  if ((slot = file_slot()) < 0) {                /* one forgotten may be garbage */
    gc_collect();
    if ((slot = file_slot()) < 0) mp_raise_OSError(MP_EMFILE);
  }
  f = mp_obj_malloc_with_finaliser(cp_file_t, bin ? &cp_fileio_type : &cp_textio_type);
  f->fd = -1;
  fd = api->open(p, flags);
  if (fd < 0 && append) fd = api->open(p, flags | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) mp_raise_OSError_with_filename(flags & CAPP_O_CREATE || append ? MP_EIO : MP_ENOENT, p);
  if (append) api->seek(fd, 0, 2);
  files[slot] = fd;
  f->fd = fd;
  return MP_OBJ_FROM_PTR(f);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

/* ---- sys.stdout, sys.stderr: the log; sys.stdin: always at its end ---------- */

static mp_uint_t stdio_read(mp_obj_t o, void *buf, mp_uint_t n, int *err) {
  (void)o;
  (void)buf;
  (void)n;
  (void)err;
  return 0;
}

static mp_uint_t stdio_write(mp_obj_t o, const void *buf, mp_uint_t n, int *err) {
  (void)o;
  (void)err;
  mp_hal_stdout_tx_strn_cooked(buf, n);
  return n;
}

static mp_uint_t stdio_ioctl(mp_obj_t o, mp_uint_t req, uintptr_t arg, int *err) {
  (void)o;
  (void)arg;
  if (req == MP_STREAM_CLOSE || req == MP_STREAM_FLUSH) return 0;
  *err = MP_EINVAL;
  return MP_STREAM_ERROR;
}

static const mp_rom_map_elem_t stdio_locals_table[] = {
  { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
  { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
  { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
  { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
};
static MP_DEFINE_CONST_DICT(stdio_locals, stdio_locals_table);
static const mp_stream_p_t stdio_stream = { .read = stdio_read, .write = stdio_write,
                                            .ioctl = stdio_ioctl, .is_text = true };
static MP_DEFINE_CONST_OBJ_TYPE(cp_stdio_type, MP_QSTR_TextIOWrapper,
                                MP_TYPE_FLAG_ITER_IS_STREAM, protocol, &stdio_stream,
                                locals_dict, &stdio_locals);

const mp_obj_base_t mp_sys_stdin_obj = { &cp_stdio_type };
const mp_obj_base_t mp_sys_stdout_obj = { &cp_stdio_type };
const mp_obj_base_t mp_sys_stderr_obj = { &cp_stdio_type };

/* ---- exit() and quit(): SystemExit, which the app answers by leaving -------- */

static mp_obj_t cp_exit(size_t n_args, const mp_obj_t *args) {
  mp_raise_type_arg(&mp_type_SystemExit, n_args ? args[0] : mp_const_none);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cardpy_exit_obj, 0, 1, cp_exit);

/* ---- os ------------------------------------------------------------------------ */

static mp_obj_t os_getcwd(void) { return mp_obj_new_str(cwd, strlen(cwd)); }
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t os_chdir(mp_obj_t path) {
  char p[PATH_MAX_];
  CappStat st;
  path_arg(path, p);
  if (api->stat(p, &st) || !st.is_dir) mp_raise_OSError_with_filename(MP_ENOENT, p);
  memcpy(cwd, p, strlen(p) + 1);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_chdir_obj, os_chdir);

static mp_obj_t os_listdir(size_t n_args, const mp_obj_t *args) {
  char p[PATH_MAX_];
  CappEntry *e;
  mp_obj_t list;
  int n, i;
  if (n_args) path_arg(args[0], p);
  else memcpy(p, cwd, strlen(cwd) + 1);
  e = m_new(CappEntry, LIST_MAX);
  n = api->list_ex(p, e, LIST_MAX);
  if (n < 0) {
    m_del(CappEntry, e, LIST_MAX);
    mp_raise_OSError_with_filename(MP_ENOENT, p);
  }
  list = mp_obj_new_list(0, NULL);
  for (i = 0; i < n; i++) mp_obj_list_append(list, mp_obj_new_str(e[i].name, strlen(e[i].name)));
  m_del(CappEntry, e, LIST_MAX);
  return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

/* stat: (mode, 0, 0, 0, 0, 0, size, 0, 0, 0), mode a folder's 0x4000 or a
 * file's 0x8000 -- what os.stat(p)[0] & 0x4000 is asked. */
static mp_obj_t os_stat(mp_obj_t path) {
  char p[PATH_MAX_];
  CappStat st;
  mp_obj_t t[10];
  int i;
  path_arg(path, p);
  if (api->stat(p, &st)) mp_raise_OSError_with_filename(MP_ENOENT, p);
  for (i = 0; i < 10; i++) t[i] = MP_OBJ_NEW_SMALL_INT(0);
  t[0] = MP_OBJ_NEW_SMALL_INT(st.is_dir ? 0x4000 : 0x8000);
  t[6] = mp_obj_new_int_from_uint(st.size);
  return mp_obj_new_tuple(10, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

static mp_obj_t os_path_call(mp_obj_t path, int (*fn)(const char *)) {
  char p[PATH_MAX_];
  path_arg(path, p);
  if (fn(p)) mp_raise_OSError_with_filename(MP_EIO, p);
  return mp_const_none;
}
static mp_obj_t os_mkdir(mp_obj_t path) { return os_path_call(path, api->mkdir); }
static MP_DEFINE_CONST_FUN_OBJ_1(os_mkdir_obj, os_mkdir);
static mp_obj_t os_remove(mp_obj_t path) { return os_path_call(path, api->remove); }
static MP_DEFINE_CONST_FUN_OBJ_1(os_remove_obj, os_remove);

static mp_obj_t os_rename(mp_obj_t from, mp_obj_t to) {
  char a[PATH_MAX_], b[PATH_MAX_];
  path_arg(from, a);
  path_arg(to, b);
  if (api->rename(a, b)) mp_raise_OSError_with_filename(MP_EIO, a);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(os_rename_obj, os_rename);

static const mp_rom_map_elem_t os_globals_table[] = {
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
  { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&os_getcwd_obj) },
  { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&os_chdir_obj) },
  { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&os_listdir_obj) },
  { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&os_stat_obj) },
  { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&os_mkdir_obj) },
  { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&os_remove_obj) },
  { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&os_rename_obj) },
};
static MP_DEFINE_CONST_DICT(os_globals, os_globals_table);
const mp_obj_module_t cardpy_module_os = { .base = { &mp_type_module },
                                           .globals = (mp_obj_dict_t *)&os_globals };
MP_REGISTER_MODULE(MP_QSTR_os, cardpy_module_os);

/* ---- time: a sleep that a held key still stops ------------------------------- */

/* Waiting in api->key_wait (API 49), which yields to the rest of the
 * machine, rather than spinning: a key typed meanwhile is the interrupt,
 * and what was printed before it is drawn. */
static void sleep_ms(mp_int_t ms) {
  uint32_t t0 = api->ticks_ms();
  for (;;) {
    mp_int_t left = ms - (mp_int_t)(api->ticks_ms() - t0);
    if (left <= 0) break;
    live(0);
    if (api->key_wait(left < 50 ? (int)left : 50) >= 0) mp_sched_keyboard_interrupt();
    mp_handle_pending(true);
  }
}

static mp_obj_t time_sleep(mp_obj_t s) {
  sleep_ms((mp_int_t)(mp_obj_get_float(s) * 1000));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(time_sleep_obj, time_sleep);

static mp_obj_t time_sleep_ms(mp_obj_t ms) {
  sleep_ms(mp_obj_get_int(ms));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(time_sleep_ms_obj, time_sleep_ms);

static mp_obj_t time_ticks_ms(void) {
  return MP_OBJ_NEW_SMALL_INT(api->ticks_ms() & (MICROPY_PY_TIME_TICKS_PERIOD - 1));
}
static MP_DEFINE_CONST_FUN_OBJ_0(time_ticks_ms_obj, time_ticks_ms);

static mp_obj_t time_ticks_diff(mp_obj_t end, mp_obj_t start) {
  mp_uint_t d = (mp_obj_get_int_truncated(end) - mp_obj_get_int_truncated(start)) &
                (MICROPY_PY_TIME_TICKS_PERIOD - 1);
  d = (d + MICROPY_PY_TIME_TICKS_PERIOD / 2) & (MICROPY_PY_TIME_TICKS_PERIOD - 1);
  return MP_OBJ_NEW_SMALL_INT((mp_int_t)d - MICROPY_PY_TIME_TICKS_PERIOD / 2);
}
static MP_DEFINE_CONST_FUN_OBJ_2(time_ticks_diff_obj, time_ticks_diff);

/* Seconds since 1970, UTC; 0 until the clock has been set. */
static mp_obj_t time_time(void) { return mp_obj_new_int_from_uint(api->epoch()); }
static MP_DEFINE_CONST_FUN_OBJ_0(time_time_obj, time_time);

static const mp_rom_map_elem_t time_globals_table[] = {
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_time) },
  { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&time_sleep_obj) },
  { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&time_sleep_ms_obj) },
  { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&time_ticks_ms_obj) },
  { MP_ROM_QSTR(MP_QSTR_ticks_diff), MP_ROM_PTR(&time_ticks_diff_obj) },
  { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&time_time_obj) },
};
static MP_DEFINE_CONST_DICT(time_globals, time_globals_table);
const mp_obj_module_t cardpy_module_time = { .base = { &mp_type_module },
                                             .globals = (mp_obj_dict_t *)&time_globals };
MP_REGISTER_MODULE(MP_QSTR_time, cardpy_module_time);
