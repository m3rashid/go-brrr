/*
 * brrr.c — a single-threaded, readiness-based webserver with a SQLite-backed
 * user CRUD API.
 *
 * Design (see proof/Brrr.lean for the formal model):
 *   - one process, one thread, zero locks, zero syscalls on shared state;
 *   - epoll (Linux) / kqueue (macOS, *BSD) readiness dispatch: work per
 *     event is O(ready), never O(connected)  [Theorem 3];
 *   - per-connection state is one `conn_t` (BUF_MAX == eventCost)  [Thm 1, 2];
 *   - SQLite runs inside the only thread: no lock contention, WAL journal;
 *   - keep-alive by default, idle connections swept so RAM tracks live users.
 *
 * API:
 *   GET    /users          list users
 *   POST   /users          create user        {"name":"...","email":"..."}
 *   GET    /users/<id>     read user
 *   PUT    /users/<id>     update user        {"name":"...","email":"..."}
 *   DELETE /users/<id>     delete user        -> 204
 *
 * Build:  make
 * Run:    ./brrr [port]          (default 8080)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/epoll.h>
#  define HAVE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#  include <sys/event.h>
#  define HAVE_KQUEUE 1
#else
#  error "brrr needs epoll (Linux) or kqueue (macOS/*BSD)"
#endif

/* ---- constants (match proof/Brrr.lean) --------------------------------- */

#define HDR_MAX        4096   /* request header cap                  */
#define BODY_MAX       2048   /* request body cap (user JSON << 2 KiB) */
#define BUF_MAX        (HDR_MAX + BODY_MAX)  /* request cap (lazily grown) */
#define INIT_BUF       512    /* request buffer starts here; grows on demand */
#define SOCK_BUF       4096   /* kernel rcv/snd buffer per connection */
#define PATH_MAX_LEN   256    /* /users/<id> fits easily; longer -> 414 */
#define LIST_LIMIT     200
#define BACKLOG        4096
#define IDLE_TIMEOUT   75     /* seconds, like nginx keepalive_timeout */
#define SWEEP_INTERVAL 5      /* seconds between idle sweeps           */
#define DB_FILE        "brrr.db"

/* ---- poller abstraction: epoll / kqueue --------------------------------- */

typedef struct {
  int fd;
#ifdef HAVE_EPOLL
  struct epoll_event *evs;
#else
  struct kevent *evs;
#endif
  int evs_cap;
} poller_t;

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) { perror("realloc"); exit(1); }
  return q;
}

static void poller_init(poller_t *p) {
#ifdef HAVE_EPOLL
  p->fd = epoll_create1(0);
#else
  p->fd = kqueue();
#endif
  if (p->fd < 0) { perror("poller create"); exit(1); }
  p->evs_cap = 1024;
  p->evs = xrealloc(NULL, (size_t)p->evs_cap * sizeof *p->evs);
}

static void poller_grow(poller_t *p, int min_cap) {
  if (min_cap <= p->evs_cap) return;
  while (p->evs_cap < min_cap) p->evs_cap *= 2;
  p->evs = xrealloc(p->evs, (size_t)p->evs_cap * sizeof *p->evs);
}

/* ---- per-connection state (one struct, BUF_MAX buffer) ------------------- */

typedef enum { ST_READ, ST_WRITE } state_t;
typedef enum { M_GET, M_HEAD, M_POST, M_PUT, M_PATCH, M_DELETE, M_OTHER } method_t;

typedef struct conn {
  int fd;
  state_t state;
  int registered;        /* poller currently holds an interest for us */
  int keep;              /* keep-alive?                               */
  method_t method;
  int is_head;           /* HEAD: send headers only                    */
  char path[PATH_MAX_LEN];
  size_t used;           /* bytes buffered                             */
  size_t consumed;       /* bytes of the request handled last          */
  size_t sent;           /* response bytes sent                        */
  char *resp;            /* response buffer (reused across requests)   */
  size_t resp_len, resp_cap;
  time_t last_active;
  struct conn *prev, *next;   /* live list, for the idle sweep          */
  char *buf;             /* request buffer: INIT_BUF, grown to BUF_MAX */
  uint32_t bufcap;       /* current capacity of buf                    */
} conn_t;

static conn_t *live_head;
static int live_count;       /* real connections currently linked */

/* grow the request buffer, always geometric (2x), capped at BUF_MAX.
 * need may be 0 (pure growth) or the size we must be able to hold.
 * returns 0 only on OOM: the caller drops the connection. */
static int conn_grow(conn_t *c, size_t need) {
  size_t cap = c->bufcap * 2;
  if (need < cap) need = cap;
  if (need > BUF_MAX) need = BUF_MAX;
  if (need <= c->bufcap) return 1;
  char *nb = realloc(c->buf, cap);
  if (!nb) return 0;
  c->buf = nb;
  c->bufcap = (uint32_t)cap;
  return 1;
}
static conn_t listener_conn; /* sentinel: poller events with this ptr => accept */

/* forward decls (used before defined) */
static void close_conn(poller_t *p, conn_t *c);
static void poller_watch(poller_t *p, conn_t *c, int want_read, int want_write);
static void poller_unwatch(poller_t *p, conn_t *c);

/* (Re)register interests for a connection. Level-triggered throughout. */
static void poller_watch(poller_t *p, conn_t *c, int want_read, int want_write) {
  if (c->registered && !want_read && !want_write) {
#ifdef HAVE_EPOLL
    epoll_ctl(p->fd, EPOLL_CTL_DEL, c->fd, NULL);
#else
    struct kevent ch[2];
    EV_SET(&ch[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->fd, ch, 2, NULL, 0, NULL);
#endif
    c->registered = 0;
    return;
  }

#ifdef HAVE_EPOLL
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = (want_read ? EPOLLIN : 0) | (want_write ? EPOLLOUT : 0);
  ev.data.ptr = c;
  epoll_ctl(p->fd, c->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, c->fd, &ev);
#else
  struct kevent ch[4];
  int n = 0;
  if (c->registered) { /* kqueue: replace filters by delete + re-add */
    EV_SET(&ch[n++], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[n++], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->fd, ch, n, NULL, 0, NULL); /* ENOENT is fine */
    n = 0;
  }
  if (want_read)  EV_SET(&ch[n++], c->fd, EVFILT_READ,  EV_ADD, 0, 0, c);
  if (want_write) EV_SET(&ch[n++], c->fd, EVFILT_WRITE, EV_ADD, 0, 0, c);
  if (n) kevent(p->fd, ch, n, NULL, 0, NULL);
#endif
  c->registered = 1;
}

static void poller_unwatch(poller_t *p, conn_t *c) {
  poller_watch(p, c, 0, 0);
}

static int poller_wait(poller_t *p) {
  int n;
#ifdef HAVE_EPOLL
  n = epoll_wait(p->fd, p->evs, p->evs_cap, -1);
#else
  n = kevent(p->fd, NULL, 0, p->evs, p->evs_cap, NULL);
#endif
  if (n < 0 && errno == EINTR) return 0;
  if (n < 0) { perror("poller_wait"); exit(1); }
  poller_grow(p, n); /* double for next round if we filled the buffer */
  return n;
}

static conn_t *ev_conn(poller_t *p, int i) {
  (void)p;
#ifdef HAVE_EPOLL
  return (conn_t *)p->evs[i].data.ptr;
#else
  return (conn_t *)p->evs[i].udata;
#endif
}

static int ev_read(poller_t *p, int i) {
#ifdef HAVE_EPOLL
  return (p->evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
#else
  return p->evs[i].filter == EVFILT_READ;
#endif
}

static int ev_write(poller_t *p, int i) {
#ifdef HAVE_EPOLL
  return (p->evs[i].events & (EPOLLOUT | EPOLLERR)) != 0;
#else
  return p->evs[i].filter == EVFILT_WRITE;
#endif
}

/* ---- dynamic string buffer (builds responses) ---------------------------- */

typedef struct { char *p; size_t n, cap, hdr; } sbuf_t;

static void sb_reserve(sbuf_t *b, size_t need) {
  if (b->n + need + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->n + need + 1) cap *= 2;
  b->p = xrealloc(b->p, cap);
  b->cap = cap;
}

static void sb_putn(sbuf_t *b, const char *s, size_t n) {
  sb_reserve(b, n);
  memcpy(b->p + b->n, s, n);
  b->n += n;
}

static void sb_put(sbuf_t *b, const char *s) { sb_putn(b, s, strlen(s)); }

static void sb_putu(sbuf_t *b, long long v) {
  char tmp[24];
  int n = snprintf(tmp, sizeof tmp, "%lld", v);
  sb_putn(b, tmp, (size_t)n);
}

/* append a JSON-escaped string (no surrounding quotes) */
static void sb_json_str(sbuf_t *b, const char *s) {
  sb_reserve(b, strlen(s) * 2 + 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { b->p[b->n++] = '\\'; b->p[b->n++] = c; }
    else if (c == '\n') { sb_putn(b, "\\n", 2); }
    else if (c == '\r') { sb_putn(b, "\\r", 2); }
    else if (c == '\t') { sb_putn(b, "\\t", 2); }
    else if (c < 0x20)  { b->n += (size_t)snprintf(b->p + b->n, 8, "\\u%04x", c); }
    else b->p[b->n++] = c;
  }
}

/* ---- tiny helpers -------------------------------------------------------- */

static void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void link_conn(conn_t *c) {
  c->prev = NULL;
  c->next = live_head;
  if (live_head) live_head->prev = c;
  live_head = c;
  live_count++;
}

static void unlink_conn(conn_t *c) {
  if (c->prev) c->prev->next = c->next; else live_head = c->next;
  if (c->next) c->next->prev = c->prev;
  c->prev = c->next = NULL;
  live_count--;
}

/* case-insensitive find of `pat` in b[0..n); returns index or -1 */
static ssize_t find_ci(const char *b, size_t n, const char *pat) {
  size_t m = strlen(pat);
  if (m == 0 || n < m) return -1;
  for (size_t i = 0; i <= n - m; i++) {
    size_t j = 0;
    while (j < m) {
      char a = b[i + j], p = pat[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
      if (a != p) break;
      j++;
    }
    if (j == m) return (ssize_t)i;
  }
  return -1;
}

/* index of the "\r\n\r\n" that ends the header block, or -1 */
static ssize_t find_header_end(const char *b, size_t n) {
  if (n < 4) return -1;
  for (size_t i = 0; i + 3 < n; i++)
    if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
      return (ssize_t)i;
  return -1;
}

/* ---- minimal JSON input (just what the user model needs) ----------------- */

static int hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

static size_t utf8_put(unsigned cp, char *o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  o[0] = (char)(0xE0 | (cp >> 12));
  o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  o[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

/* Extract a string field `key` from the JSON object s[0..n) into out.
 * Handles \" \\ \/ \b \f \n \r \t and \uXXXX (incl. surrogate pairs).
 * Returns decoded length and NUL-terminates, or -1. */
static ssize_t json_get_string(const char *s, size_t n, const char *key,
                               char *out, size_t outcap) {
  size_t klen = strlen(key);
  for (size_t i = 0; i + klen + 2 < n; i++) {
    if (s[i] != '"') continue;
    if (memcmp(s + i + 1, key, klen) != 0 || s[i + 1 + klen] != '"') continue;
    size_t j = i + 1 + klen + 1;
    while (j < n && isspace((unsigned char)s[j])) j++;
    if (j >= n || s[j] != ':') continue;
    j++;
    while (j < n && isspace((unsigned char)s[j])) j++;
    if (j >= n || s[j] != '"') continue;
    j++;
    size_t o = 0;
    while (j < n && s[j] != '"') {
      char one[4];
      size_t one_n = 0;
      if (s[j] == '\\' && j + 1 < n) {
        j++;
        char e = s[j++];
        switch (e) {
        case '"': case '\\': case '/': one[one_n++] = e; break;
        case 'n': one[one_n++] = '\n'; break;
        case 'r': one[one_n++] = '\r'; break;
        case 't': one[one_n++] = '\t'; break;
        case 'b': one[one_n++] = '\b'; break;
        case 'f': one[one_n++] = '\f'; break;
        case 'u': {
          unsigned cp;
          if (j + 4 > n || hex4(s + j, &cp) < 0) return -1;
          j += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && j + 6 <= n &&
              s[j] == '\\' && s[j + 1] == 'u') {
            unsigned lo;
            if (hex4(s + j + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              j += 6;
            } else cp = '?';
          } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';
          one_n = utf8_put(cp, one);
          break;
        }
        default: one[one_n++] = e; break;
        }
      } else {
        one[one_n++] = s[j++];
      }
      if (o + one_n > outcap) return -1;   /* field too long */
      memcpy(out + o, one, one_n);
      o += one_n;
    }
    if (j < n) { out[o] = 0; return (ssize_t)o; }
    return -1; /* unterminated string */
  }
  return -1; /* key not found */
}

/* ---- database (SQLite, single-threaded: zero lock contention) ------------- */

static sqlite3 *db;
static sqlite3_stmt *st_ins, *st_sel, *st_upd, *st_del, *st_list;

typedef struct {
  long long id, created_at;
  char name[256], email[256];
} user_t;

static void die_sql(const char *what) {
  fprintf(stderr, "sqlite %s failed: %s\n", what, sqlite3_errmsg(db));
  exit(1);
}

static void db_open(void) {
  int rc = sqlite3_open(DB_FILE, &db);
  if (rc != SQLITE_OK) die_sql("open");
  sqlite3_busy_timeout(db, 2000);
  char *err = NULL;
  rc = sqlite3_exec(db,
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA foreign_keys=ON;"
      /* cheap-RAM tuning: bounded page cache, small WAL, mmap the db file */
      "PRAGMA cache_size=-8000;"          /* 8 MiB page cache per worker */
      "PRAGMA mmap_size=134217728;"       /* file-backed reads via mmap  */
      "PRAGMA wal_autocheckpoint=128;"    /* keep the WAL ~512 KiB       */
      "PRAGMA temp_store=MEMORY;"
      "CREATE TABLE IF NOT EXISTS users ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " name TEXT NOT NULL,"
      " email TEXT NOT NULL,"
      " created_at INTEGER NOT NULL);",
      NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite exec: %s\n", err ? err : sqlite3_errmsg(db));
    exit(1);
  }
  sqlite3_free(err);

  rc = sqlite3_prepare_v2(db,
      "INSERT INTO users(name,email,created_at) VALUES(?,?,?);", -1, &st_ins, NULL);
  if (rc != SQLITE_OK) die_sql("prepare insert");
  rc = sqlite3_prepare_v2(db,
      "SELECT name,email,created_at FROM users WHERE id=?;", -1, &st_sel, NULL);
  if (rc != SQLITE_OK) die_sql("prepare select");
  rc = sqlite3_prepare_v2(db,
      "UPDATE users SET name=?, email=? WHERE id=?;", -1, &st_upd, NULL);
  if (rc != SQLITE_OK) die_sql("prepare update");
  rc = sqlite3_prepare_v2(db,
      "DELETE FROM users WHERE id=?;", -1, &st_del, NULL);
  if (rc != SQLITE_OK) die_sql("prepare delete");
  rc = sqlite3_prepare_v2(db,
      "SELECT id,name,email,created_at FROM users ORDER BY id LIMIT ?;", -1,
      &st_list, NULL);
  if (rc != SQLITE_OK) die_sql("prepare list");
}

/* all four CRUD behaviours */
static int db_create(const char *name, const char *email, user_t *u) {
  sqlite3_reset(st_ins);
  sqlite3_bind_text(st_ins, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st_ins, 2, email, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st_ins, 3, (sqlite3_int64)time(NULL));
  int rc = sqlite3_step(st_ins);
  int ok = (rc == SQLITE_DONE);
  if (ok) {
    u->id = sqlite3_last_insert_rowid(db);
    u->created_at = (long long)time(NULL);
    snprintf(u->name, sizeof u->name, "%s", name);
    snprintf(u->email, sizeof u->email, "%s", email);
  }
  sqlite3_reset(st_ins);
  return ok;
}

static int db_read(long long id, user_t *u) { /* 1 = found, 0 = not found, -1 = err */
  sqlite3_reset(st_sel);
  sqlite3_bind_int64(st_sel, 1, id);
  int rc = sqlite3_step(st_sel);
  int res;
  if (rc == SQLITE_ROW) {
    snprintf(u->name, sizeof u->name, "%s", (const char *)sqlite3_column_text(st_sel, 0));
    snprintf(u->email, sizeof u->email, "%s", (const char *)sqlite3_column_text(st_sel, 1));
    u->created_at = sqlite3_column_int64(st_sel, 2);
    u->id = id;
    res = 1;
  } else if (rc == SQLITE_DONE) {
    res = 0;
  } else {
    res = -1;
  }
  sqlite3_reset(st_sel);
  return res;
}

static int db_update(long long id, const char *name, const char *email,
                     user_t *u) { /* 1 = updated, 0 = not found, -1 = err */
  sqlite3_reset(st_upd);
  sqlite3_bind_text(st_upd, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st_upd, 2, email, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st_upd, 3, id);
  int rc = sqlite3_step(st_upd);
  int changes = sqlite3_changes(db);
  sqlite3_reset(st_upd);
  if (rc != SQLITE_DONE) return -1;
  if (changes == 0) return 0;
  return db_read(id, u);
}

static int db_delete(long long id) { /* 1 = deleted, 0 = not found, -1 = err */
  sqlite3_reset(st_del);
  sqlite3_bind_int64(st_del, 1, id);
  int rc = sqlite3_step(st_del);
  int changes = sqlite3_changes(db);
  sqlite3_reset(st_del);
  if (rc != SQLITE_DONE) return -1;
  return changes > 0;
}

/* append {"users":[...]} to b; 1 = ok, -1 = err */
static int db_list(sbuf_t *b) {
  sqlite3_reset(st_list);
  sqlite3_bind_int(st_list, 1, LIST_LIMIT);
  sb_put(b, "{\"users\":[");
  int rc, first = 1;
  while ((rc = sqlite3_step(st_list)) == SQLITE_ROW) {
    if (!first) sb_put(b, ",");
    first = 0;
    sb_put(b, "{\"id\":");
    sb_putu(b, sqlite3_column_int64(st_list, 0));
    sb_put(b, ",\"name\":\"");
    sb_json_str(b, (const char *)sqlite3_column_text(st_list, 1));
    sb_put(b, "\",\"email\":\"");
    sb_json_str(b, (const char *)sqlite3_column_text(st_list, 2));
    sb_put(b, "\",\"created_at\":");
    sb_putu(b, sqlite3_column_int64(st_list, 3));
    sb_put(b, "}");
  }
  sqlite3_reset(st_list);
  sb_put(b, "]}\r\n");
  return rc == SQLITE_DONE ? 0 : -1;
}

/* ---- HTTP response plumbing ---------------------------------------------- */


/* build a response into c->resp from an sbuf over it; `extra` = optional
 * additional header lines (each ending in \r\n), placed before Server: */
static void resp_begin(sbuf_t *b, conn_t *c, const char *status, int keep,
                       const char *extra) {
  b->p = c->resp;
  b->n = 0;
  b->cap = c->resp_cap;
  sb_reserve(b, 512);
  b->n = (size_t)snprintf(b->p, 512,
      "%s%sServer: brrr\r\nContent-Type: application/json\r\n"
      "Content-Length: ", status, extra ? extra : "");
      /* status already carries its own \r\n */
  b->hdr = b->n; /* body starts here; status length may vary */
  c->keep = keep;
}

static void resp_end(conn_t *c, sbuf_t *b) {
  /* splice the body length right after "Content-Length: " */
  char clen[32];
  size_t body_len = b->n - b->hdr;
  int n = snprintf(clen, sizeof clen, "%zu\r\nConnection: %s\r\n\r\n",
                   body_len, c->keep ? "keep-alive" : "close");
  sb_reserve(b, (size_t)n);
  memmove(b->p + b->hdr + (size_t)n, b->p + b->hdr, b->n - b->hdr);
  memcpy(b->p + b->hdr, clen, (size_t)n);
  b->n += (size_t)n;
  c->resp = b->p;
  c->resp_len = b->n;
  c->resp_cap = b->cap;
  if (c->is_head) { /* HEAD: headers only, Content-Length stays correct */
    for (size_t i = 0; i + 3 < b->n; i++)
      if (b->p[i] == '\r' && b->p[i+1] == '\n' && b->p[i+2] == '\r' && b->p[i+3] == '\n') {
        c->resp_len = i + 4;
        break;
      }
  }
  c->sent = 0;
}

/* simple non-json error (shared code path) */
static void resp_simple(conn_t *c, const char *status, const char *body) {
  sbuf_t b;
  resp_begin(&b, c, status, c->keep, NULL);
  sb_put(&b, body);
  resp_end(c, &b);
}

#define JSON_ERR(c, code, msg) \
  resp_simple(c, "HTTP/1.1 " #code " \r\n", "{\"error\":\"" msg "\"}\r\n")

/* ---- request handling ------------------------------------------------------ */

static void pump(poller_t *p, conn_t *c); /* fwd */

/* one buffered, complete request -> response (DB work happens here) */
static void handle_request(poller_t *p, conn_t *c, const char *body, size_t blen) {
  sbuf_t b;
  const char *path = c->path;
  method_t m = c->method;

  /* observability: per-worker RSS + live connection count */
  if (strcmp(path, "/_stats") == 0) {
    if (m != M_GET && m != M_HEAD) { JSON_ERR(c, 405 Method Not Allowed, "method not allowed"); return; }
    long rss_kb = 0, cg_kb = 0;
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
      char line[128];
      while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) break;
      fclose(f);
    }
    f = fopen("/sys/fs/cgroup/memory.current", "r");
    if (f) { if (fscanf(f, "%ld", &cg_kb) != 1) cg_kb = 0; fclose(f); }
#endif
    resp_begin(&b, c, "HTTP/1.1 200 OK\r\n", c->keep, NULL);
    sb_reserve(&b, 256);
    b.n += (size_t)snprintf(b.p + b.n, 256,
        "{\"pid\":%d,\"rss_kb\":%ld,\"cgroup_kb\":%ld,\"conns\":%d,\"conn_bytes\":%zu",
        getpid(), rss_kb, cg_kb / 1024, live_count, sizeof(conn_t));
    sb_put(&b, "}\r\n");
    resp_end(c, &b);
    return;
  }

  if (strcmp(path, "/") == 0) {
    if (m != M_GET && m != M_HEAD) { JSON_ERR(c, 405 Method Not Allowed, "method not allowed"); return; }
    resp_begin(&b, c, "HTTP/1.1 200 OK\r\n", c->keep, NULL);
    sb_put(&b, "{\"service\":\"brrr\",\"db\":\"sqlite\",\"users\":\"/users\"}\r\n");
    resp_end(c, &b);
    return;
  }

  if (strcmp(path, "/users") == 0) {
    if (m == M_GET || m == M_HEAD) {
      resp_begin(&b, c, "HTTP/1.1 200 OK\r\n", c->keep, NULL);
      if (db_list(&b) < 0) { /* db_list may have realloc'd the buffer */
        c->resp = b.p; c->resp_cap = b.cap;
        JSON_ERR(c, 500 Internal Server Error, "db error");
        return;
      }
      resp_end(c, &b);
      return;
    }
    if (m == M_POST) {
      char name[256], email[256];
      if (json_get_string(body, blen, "name", name, sizeof name - 1) < 0 ||
          json_get_string(body, blen, "email", email, sizeof email - 1) < 0) {
        JSON_ERR(c, 400 Bad Request, "need JSON body with name and email");
        return;
      }
      user_t u;
      if (!db_create(name, email, &u)) { JSON_ERR(c, 500 Internal Server Error, "db error"); return; }
      char loc[64];
      snprintf(loc, sizeof loc, "Location: /users/%lld\r\n", u.id);
      resp_begin(&b, c, "HTTP/1.1 201 Created\r\n", c->keep, loc);
      sb_put(&b, "{\"id\":");
      sb_putu(&b, u.id);
      sb_put(&b, ",\"name\":\"");
      sb_json_str(&b, name);
      sb_put(&b, "\",\"email\":\"");
      sb_json_str(&b, email);
      sb_put(&b, "\",\"created_at\":");
      sb_putu(&b, u.created_at);
      sb_put(&b, "}\r\n");
      resp_end(c, &b);
      return;
    }
    JSON_ERR(c, 405 Method Not Allowed, "method not allowed");
    return;
  }

  if (strncmp(path, "/users/", 7) == 0) {
    char *end;
    long long id = strtoll(path + 7, &end, 10);
    if (*end != 0 || id <= 0) { JSON_ERR(c, 404 Not Found, "no such user"); return; }

    if (m == M_GET || m == M_HEAD) {
      user_t u;
      int r = db_read(id, &u);
      if (r < 0) { JSON_ERR(c, 500 Internal Server Error, "db error"); return; }
      if (r == 0) { JSON_ERR(c, 404 Not Found, "no such user"); return; }
      resp_begin(&b, c, "HTTP/1.1 200 OK\r\n", c->keep, NULL);
      sb_put(&b, "{\"id\":");
      sb_putu(&b, u.id);
      sb_put(&b, ",\"name\":\"");
      sb_json_str(&b, u.name);
      sb_put(&b, "\",\"email\":\"");
      sb_json_str(&b, u.email);
      sb_put(&b, "\",\"created_at\":");
      sb_putu(&b, u.created_at);
      sb_put(&b, "}\r\n");
      resp_end(c, &b);
      return;
    }
    if (m == M_PUT || m == M_PATCH) {
      char name[256], email[256];
      if (json_get_string(body, blen, "name", name, sizeof name - 1) < 0 ||
          json_get_string(body, blen, "email", email, sizeof email - 1) < 0) {
        JSON_ERR(c, 400 Bad Request, "need JSON body with name and email");
        return;
      }
      user_t u;
      int r = db_update(id, name, email, &u);
      if (r < 0) { JSON_ERR(c, 500 Internal Server Error, "db error"); return; }
      if (r == 0) { JSON_ERR(c, 404 Not Found, "no such user"); return; }
      resp_begin(&b, c, "HTTP/1.1 200 OK\r\n", c->keep, NULL);
      sb_put(&b, "{\"id\":");
      sb_putu(&b, u.id);
      sb_put(&b, ",\"name\":\"");
      sb_json_str(&b, u.name);
      sb_put(&b, "\",\"email\":\"");
      sb_json_str(&b, u.email);
      sb_put(&b, "\",\"created_at\":");
      sb_putu(&b, u.created_at);
      sb_put(&b, "}\r\n");
      resp_end(c, &b);
      return;
    }
    if (m == M_DELETE) {
      int r = db_delete(id);
      if (r < 0) { JSON_ERR(c, 500 Internal Server Error, "db error"); return; }
      if (r == 0) { JSON_ERR(c, 404 Not Found, "no such user"); return; }
      resp_simple(c, "HTTP/1.1 204 No Content\r\n", ""); /* 204: no body */
      return;
    }
    JSON_ERR(c, 405 Method Not Allowed, "method not allowed");
    return;
  }

  JSON_ERR(c, 404 Not Found, "not found");
  (void)p;
}

/* parse the buffered request, if complete. returns:
 *   0 = need more bytes, 1 = request handled (response queued), -1 = drop conn */
static int parse_and_handle(poller_t *p, conn_t *c) {
  ssize_t he = find_header_end(c->buf, c->used);
  if (he < 0) {
    if (c->used >= HDR_MAX) { /* headers too big */
      c->keep = 0;
      resp_simple(c, "HTTP/1.1 431 Request Header Fields Too Large\r\n",
                  "{\"error\":\"headers too large\"}\r\n");
      c->consumed = c->used;
      c->used = 0;
      c->state = ST_WRITE;
      return 1;
    }
    return 0; /* need more */
  }

  size_t hdr = (size_t)he + 4;

  /* request line: METHOD SP PATH SP ... */
  char *sp1 = memchr(c->buf, ' ', he);
  if (!sp1) { c->keep = 0; return -1; }
  size_t mlen = (size_t)(sp1 - c->buf);
  char *sp2 = memchr(sp1 + 1, ' ', (size_t)(he - mlen - 1));
  if (!sp2) { c->keep = 0; return -1; }
  size_t plen = (size_t)(sp2 - (sp1 + 1));
  if (plen >= PATH_MAX_LEN) {
    c->keep = 0;
    resp_simple(c, "HTTP/1.1 414 URI Too Long\r\n", "{\"error\":\"uri too long\"}\r\n");
    c->consumed = c->used; c->used = 0; c->state = ST_WRITE;
    return 1;
  }
  memcpy(c->path, sp1 + 1, plen);
  c->path[plen] = 0;
  char *q = strchr(c->path, '?'); /* strip query for now */
  if (q) *q = 0;

  c->method = M_OTHER;
  if (mlen == 3 && memcmp(c->buf, "GET", 3) == 0) c->method = M_GET;
  else if (mlen == 4 && memcmp(c->buf, "HEAD", 4) == 0) c->method = M_HEAD;
  else if (mlen == 4 && memcmp(c->buf, "POST", 4) == 0) c->method = M_POST;
  else if (mlen == 3 && memcmp(c->buf, "PUT", 3) == 0) c->method = M_PUT;
  else if (mlen == 5 && memcmp(c->buf, "PATCH", 5) == 0) c->method = M_PATCH;
  else if (mlen == 6 && memcmp(c->buf, "DELETE", 6) == 0) c->method = M_DELETE;
  c->is_head = (c->method == M_HEAD);

  c->keep = find_ci(c->buf, hdr, "connection: close") < 0;

  /* Content-Length (headers only) */
  size_t clen = 0;
  ssize_t cl = find_ci(c->buf, hdr, "content-length:");
  if (cl >= 0) {
    const char *p = c->buf + cl + 15;
    const char *pe = c->buf + he;
    while (p < pe && (*p == ' ' || *p == '\t')) p++;
    clen = 0;
    while (p < pe && *p >= '0' && *p <= '9') { clen = clen * 10 + (size_t)(*p - '0'); p++; }
  }
  if (clen > BODY_MAX) {
    c->keep = 0;
    resp_simple(c, "HTTP/1.1 413 Content Too Large\r\n", "{\"error\":\"body too large\"}\r\n");
    c->consumed = c->used; c->used = 0; c->state = ST_WRITE;
    return 1;
  }
  if (c->used < hdr + clen) { /* body not fully arrived yet */
    if (!conn_grow(c, hdr + clen)) return -1; /* OOM */
    return 0;
  }

  c->consumed = hdr + clen;
  handle_request(p, c, c->buf + hdr, clen);
  c->state = ST_WRITE;
  return 1;
}

/* central pump: write pending response, then parse next buffered request.
 * Always leaves the poller interests correct for c. */
static void pump(poller_t *p, conn_t *c) {
  for (;;) {
    if (c->state == ST_WRITE) {
      /* flush response */
      while (c->sent < c->resp_len) {
        ssize_t n = send(c->fd, c->resp + c->sent, c->resp_len - c->sent, MSG_NOSIGNAL);
        if (n > 0) { c->sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          poller_watch(p, c, 0, 1);
          return;
        }
        close_conn(p, c);
        return;
      }
      /* response complete */
      if (!c->keep) { close_conn(p, c); return; }
      c->used -= c->consumed; /* drop handled request, keep pipelined bytes */
      if (c->used > 0) memmove(c->buf, c->buf + c->consumed, c->used);
      else c->used = 0;
      c->state = ST_READ;
      c->last_active = time(NULL);
    }

    if (c->used > 0) {
      int r = parse_and_handle(p, c);
      if (r < 0) { close_conn(p, c); return; }
      if (r == 0) { poller_watch(p, c, 1, 0); return; }
      continue; /* response queued: loop to flush it */
    }
    poller_watch(p, c, 1, 0);
    return;
  }
}

static void on_readable(poller_t *p, conn_t *c) {
  for (;;) {
    if (c->used == c->bufcap) {
      if (c->bufcap == BUF_MAX) break; /* full: let pump decide (413/431) */
      if (!conn_grow(c, 0)) { close_conn(p, c); return; } /* OOM */
    }
    ssize_t n = recv(c->fd, c->buf + c->used, c->bufcap - c->used, 0);
    if (n > 0) { c->used += (size_t)n; continue; }
    if (n == 0) { close_conn(p, c); return; } /* client hung up */
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (errno == EINTR) continue;
    close_conn(p, c);
    return;
  }
  c->last_active = time(NULL);
  pump(p, c);
}

static void on_writable(poller_t *p, conn_t *c) {
  if (c->state == ST_WRITE) pump(p, c);
  else poller_watch(p, c, 1, 0);
}

/* ---- connection lifecycle ------------------------------------------------ */

static void close_conn(poller_t *p, conn_t *c) {
  if (c == &listener_conn) return;
  unlink_conn(c);
  if (c->registered) poller_unwatch(p, c);
  close(c->fd);
  free(c->buf);
  free(c);
}

static void accept_all(poller_t *p, int s) {
  for (;;) {
#ifdef HAVE_EPOLL
    int fd = accept4(s, NULL, NULL, SOCK_NONBLOCK);
#else
    int fd = accept(s, NULL, NULL);
#endif
    if (fd < 0) {
      if (errno == EINTR) continue;
      return; /* EAGAIN etc.: no more pending connections */
    }
#ifndef HAVE_EPOLL
    set_nonblock(fd);
#endif
    int one = 1, one_4k = SOCK_BUF;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* kernel-level backstop for dead peers between app sweeps */
#ifdef TCP_KEEPIDLE
    int idle = IDLE_TIMEOUT, intvl = 10, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#elif defined(TCP_KEEPALIVE)
    int idle = IDLE_TIMEOUT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);
#endif
    int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof enable);
    /* every socket buffer is per-user memory on a cheap box: cap both.
     * Responses are small; bodies arrive in small TCP windows. */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &one_4k, sizeof one_4k);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &one_4k, sizeof one_4k);

    conn_t *c = calloc(1, sizeof *c);
    if (!c) { close(fd); continue; } /* out of RAM: drop, keep serving */
    c->buf = malloc(INIT_BUF); /* lazy: only what a real request touches */
    if (!c->buf) { free(c); close(fd); continue; }
    c->bufcap = INIT_BUF;
    c->fd = fd;
    c->state = ST_READ;
    c->last_active = time(NULL);
    link_conn(c);
    poller_watch(p, c, 1, 0);
  }
}

/* close keep-alive connections idle for too long, so RAM tracks live users */
static void sweep(poller_t *p, time_t now) {
  conn_t *c = live_head;
  while (c) {
    conn_t *next = c->next;
    if (now - c->last_active > IDLE_TIMEOUT) close_conn(p, c);
    c = next;
  }
}

/* ---- worker: one process = one event loop = one core ---------------------- */

static void run_worker(int port) {
  /* SQLite must not be shared across fork(): each worker opens its own */
  db_open();

  /* use every fd the kernel allows */
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  int s = socket(AF_INET6, SOCK_STREAM, 0);
  int family = AF_INET6;
  if (s < 0) { family = AF_INET; s = socket(AF_INET, SOCK_STREAM, 0); }
  if (s < 0) { perror("socket"); exit(1); }

  int one = 1, zero = 0;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  /* SO_REUSEPORT: every worker binds the same port; the kernel hashes
   * incoming connections across listeners => one hot accept queue per core */
#ifdef SO_REUSEPORT
  setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  if (family == AF_INET6)
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);

  if (family == AF_INET6) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
  } else {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
  }

  int backlog = BACKLOG;
#ifdef SOMAXCONN
  if (SOMAXCONN > 0 && SOMAXCONN < backlog) backlog = SOMAXCONN;
#endif
  if (listen(s, backlog) < 0) { perror("listen"); exit(1); }
  set_nonblock(s);

  poller_t p;
  poller_init(&p);

  memset(&listener_conn, 0, sizeof listener_conn);
  listener_conn.fd = s;
  listener_conn.state = ST_READ;
  listener_conn.last_active = time(NULL);
  poller_watch(&p, &listener_conn, 1, 0);

  fprintf(stderr, "brrr[%d]: listening on [::]:%d (1 thread, 0 locks, "
                  "%zu B/conn, sqlite %s)\n", getpid(), port, sizeof(conn_t),
                  sqlite3_libversion());

  time_t last_sweep = time(NULL);
  for (;;) {
    int n = poller_wait(&p);
    time_t now = time(NULL);
    if (now - last_sweep >= SWEEP_INTERVAL) {
      sweep(&p, now);
      last_sweep = now;
    }
    for (int i = 0; i < n; i++) {
      conn_t *c = ev_conn(&p, i);
      if (c == &listener_conn) { accept_all(&p, s); continue; }
      if (ev_write(&p, i)) on_writable(&p, c);
      else if (ev_read(&p, i)) on_readable(&p, c);
    }
  }
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8080;
  int workers = argc > 2 ? atoi(argv[2]) : 1;
  if (workers < 1) workers = 1;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN); /* workers are fire-and-forget: no zombies */

  /* fork workers-1 extra event loops; the parent runs one too. Each worker
   * is a full Theorem-1..3 instance: 1 thread, 0 locks, floor memory. */
  for (int i = 1; i < workers; i++)
    if (fork() == 0) break; /* child: stop forking, become a worker */

  run_worker(port);
  return 0;
}
