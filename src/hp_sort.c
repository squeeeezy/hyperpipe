#include "hyperpipe/hp_sort.h"
#include "hyperpipe/hp_filter.h"
#include "hp_number.h"
#include "hp_thread.h"

/* Records without a limit are carved out of large blocks: one allocation
 * per block instead of one per row. */
struct hp_sort_block_s {
    hp_sort_block_t *next;
    size_t used, cap;
    uint8_t data[];
};

#define BLOCK_SIZE ((size_t)1 << 20)

/* A sorted run: rows in memory, or a temporary file of records */
struct hp_sort_run_s {
    hp_sort_entry_t *entries;
    size_t count;
    hp_sort_block_t *blocks;
    bool own_recs;         /* records allocated one by one (with a limit) */
    bool borrowed;         /* entries and records belong to an earlier run */
    size_t bytes;
    FILE *file;
    int level;             /* merges it took to build: runs merge a level at a time */
};

/* Numbers, then text, then empty values, in either direction: a descending
 * sort of a column of numbers with stray text still starts with the
 * largest number. The direction orders values within each kind. */
#define TAG_NUMBER 0x01
#define TAG_TEXT   0x02
#define TAG_EMPTY  0xFF

/* ---- records ------------------------------------------------------------------ */

static uint32_t rec_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static const uint8_t *rec_key(const uint8_t *rec) { return rec + 8; }
static size_t rec_key_len(const uint8_t *rec) { return rec_u32(rec); }
static const uint8_t *rec_line(const uint8_t *rec) { return rec + 8 + rec_u32(rec); }
static size_t rec_line_len(const uint8_t *rec) { return rec_u32(rec + 4); }
static size_t rec_size(const uint8_t *rec) { return 8 + rec_u32(rec) + rec_u32(rec + 4); }

static void set_prefix(hp_sort_entry_t *e, const uint8_t *key, size_t len) {
    uint8_t padded[16] = { 0 };
    memcpy(padded, key, len < 16 ? len : 16);
    for (int w = 0; w < 2; w++) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | padded[8 * w + i];
        e->prefix[w] = v;
    }
}

HP_INLINE int prefix_cmp(const hp_sort_entry_t *a, const hp_sort_entry_t *b) {
    if (a->prefix[0] != b->prefix[0]) return a->prefix[0] < b->prefix[0] ? -1 : 1;
    if (a->prefix[1] != b->prefix[1]) return a->prefix[1] < b->prefix[1] ? -1 : 1;
    return 0;
}

/* Two keys whose prefixes are equal, compared past the prefix */
static int tail_cmp(const uint8_t *ka, size_t la, const uint8_t *kb, size_t lb) {
    size_t n = la < lb ? la : lb;
    if (n > 16) {
        int c = memcmp(ka + 16, kb + 16, n - 16);
        if (c != 0) return c;
    }
    return (la > lb) - (la < lb);
}

/* Past the prefix only when some key is longer: key encodings delimit
 * themselves, so two keys that fit the prefix are equal when it is */
HP_INLINE int entry_cmp(const hp_sort_entry_t *a, const hp_sort_entry_t *b, bool long_keys) {
    int c = prefix_cmp(a, b);
    if (c != 0 || !long_keys) return c;
    return tail_cmp(rec_key(a->rec), rec_key_len(a->rec), rec_key(b->rec), rec_key_len(b->rec));
}

HP_INLINE bool entry_less(const hp_sort_entry_t *a, const hp_sort_entry_t *b, bool long_keys) {
    return entry_cmp(a, b, long_keys) < 0;
}

static void free_blocks(hp_sort_block_t *b) {
    while (b) {
        hp_sort_block_t *next = b->next;
        free(b);
        b = next;
    }
}

static void free_run(hp_sort_run_t *run) {
    if (run->own_recs) {
        for (size_t i = 0; i < run->count; i++) free(run->entries[i].rec);
    }
    if (!run->borrowed) {
        free(run->entries);
        free_blocks(run->blocks);
    }
    if (run->file) fclose(run->file);
    memset(run, 0, sizeof(*run));
}

/* ---- keys --------------------------------------------------------------------- */

static hp_error_t key_reserve(hp_sorter_t *s, size_t need) {
    if (need <= s->key_cap) return HP_OK;
    size_t cap = s->key_cap ? s->key_cap : 256;
    while (cap < need) cap *= 2;
    uint8_t *key = (uint8_t *)realloc(s->key, cap);
    if (!key) return HP_ERR_NOMEM;
    s->key = key;
    s->key_cap = cap;
    return HP_OK;
}

static bool field_number(const hp_str_t *f, double *v) {
    int64_t i;
    if (f->is_json) return false;
    if (hp_parse_int_span(f->ptr, f->len, &i)) {
        *v = (double)i;
        return true;
    }
    return hp_parse_decimal_span(f->ptr, f->len, v);
}

/* Builds the row's key in s->key and returns its length */
static hp_error_t build_key(hp_sorter_t *s, const hp_row_t *row, size_t *key_len) {
    size_t need = 8;
    for (size_t k = 0; k < s->key_count; k++) {
        size_t idx = (size_t)s->keys[k].col_index;
        need += 9 + (idx < row->field_count ? 2 * row->fields[idx].len + 2 : 0);
    }
    if (key_reserve(s, need) != HP_OK) return HP_ERR_NOMEM;

    uint8_t *w = s->key;
    for (size_t k = 0; k < s->key_count; k++) {
        size_t idx = (size_t)s->keys[k].col_index;
        const uint8_t flip = s->keys[k].desc ? 0xFF : 0x00;
        const hp_str_t *f = idx < row->field_count && row->fields[idx].present &&
                            row->fields[idx].len > 0 ? &row->fields[idx] : NULL;
        double v;
        if (!f) {
            *w++ = TAG_EMPTY;
        } else if (field_number(f, &v)) {
            /* IEEE order as unsigned: flip the sign bit of positives and
             * every bit of negatives; -0 sorts with 0 */
            uint64_t bits;
            if (v == 0) v = 0;
            memcpy(&bits, &v, 8);
            bits = (bits >> 63) ? ~bits : bits | 0x8000000000000000ull;
            *w++ = TAG_NUMBER;
            for (int i = 7; i >= 0; i--) *w++ = (uint8_t)(bits >> (8 * i)) ^ flip;
        } else {
            /* A zero byte is escaped as 00 FF and the text ends with 00 00,
             * so a prefix sorts before every longer text */
            *w++ = TAG_TEXT;
            if (!flip && !memchr(f->ptr, 0, f->len)) {
                memcpy(w, f->ptr, f->len);
                w += f->len;
            } else {
                for (size_t i = 0; i < f->len; i++) {
                    uint8_t c = (uint8_t)f->ptr[i];
                    *w++ = c ^ flip;
                    if (c == 0) *w++ = 0xFF ^ flip;
                }
            }
            *w++ = flip;
            *w++ = flip;
        }
    }
    if (s->limit) {
        for (int i = 7; i >= 0; i--) *w++ = (uint8_t)(s->seq >> (8 * i));
    }
    s->seq++;
    *key_len = (size_t)(w - s->key);
    s->long_keys |= *key_len > 16;
    return HP_OK;
}

static void put_rec(uint8_t *rec, const uint8_t *key, size_t key_len, const void *line, size_t len) {
    uint32_t kl = (uint32_t)key_len, ll = (uint32_t)len;
    memcpy(rec, &kl, 4);
    memcpy(rec + 4, &ll, 4);
    memcpy(rec + 8, key, key_len);
    memcpy(rec + 8 + key_len, line, len);
}

/* ---- sorting ------------------------------------------------------------------ */

/* One bottom-up pass: merges pairs of sorted runs of width w from src into
 * dst. Ties take the left run, which keeps the sort stable. */
static void merge_pass(const hp_sort_entry_t *src, hp_sort_entry_t *dst, size_t n, size_t width,
                       bool long_keys) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
        size_t mid = lo + width < n ? lo + width : n;
        size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
        size_t i = lo, j = mid, k = lo;
        while (i < mid && j < hi) {
            dst[k++] = entry_less(&src[j], &src[i], long_keys) ? src[j++] : src[i++];
        }
        while (i < mid) dst[k++] = src[i++];
        while (j < hi) dst[k++] = src[j++];
    }
}

/* Sorts e[0..n) with tmp as scratch, leaving the result in e */
static void sort_span(hp_sort_entry_t *e, hp_sort_entry_t *tmp, size_t n, size_t from_width,
                      bool long_keys) {
    hp_sort_entry_t *src = e, *dst = tmp;
    for (size_t width = from_width; width < n; width *= 2) {
        merge_pass(src, dst, n, width, long_keys);
        hp_sort_entry_t *t = src;
        src = dst;
        dst = t;
    }
    if (src != e) memcpy(e, src, n * sizeof(*e));
}

/* Stable merge sort. Blocks that fit in cache are sorted first, so most of
 * the passes stay in cache; only the last few stream the whole array. */
static hp_error_t sort_entries(hp_sort_entry_t *e, size_t n, bool long_keys) {
    if (n < 2) return HP_OK;
    hp_sort_entry_t *tmp = (hp_sort_entry_t *)malloc(n * sizeof(*tmp));
    if (!tmp) return HP_ERR_NOMEM;

    const size_t small = 16, block = 16384;
    for (size_t lo = 0; lo < n; lo += small) {
        size_t hi = lo + small < n ? lo + small : n;
        for (size_t i = lo + 1; i < hi; i++) {
            hp_sort_entry_t x = e[i];
            size_t j = i;
            while (j > lo && entry_less(&x, &e[j - 1], long_keys)) {
                e[j] = e[j - 1];
                j--;
            }
            e[j] = x;
        }
    }
    for (size_t lo = 0; lo < n; lo += block) {
        size_t len = lo + block < n ? block : n - lo;
        sort_span(e + lo, tmp + lo, len, small, long_keys);
    }
    sort_span(e, tmp, n, block, long_keys);
    free(tmp);
    return HP_OK;
}

/* Max-heap on the key: with a limit the root is the row to drop next */
static void heap_up(hp_sort_entry_t *h, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!entry_less(&h[p], &h[i], true)) break;
        hp_sort_entry_t t = h[p];
        h[p] = h[i];
        h[i] = t;
        i = p;
    }
}

static void heap_down(hp_sort_entry_t *h, size_t n, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && entry_less(&h[m], &h[l], true)) m = l;
        if (r < n && entry_less(&h[m], &h[r], true)) m = r;
        if (m == i) return;
        hp_sort_entry_t t = h[m];
        h[m] = h[i];
        h[i] = t;
        i = m;
    }
}

/* ---- runs --------------------------------------------------------------------- */

static hp_error_t push_run(hp_sorter_t *s, const hp_sort_run_t *run) {
    if (s->run_count == s->run_cap) {
        size_t cap = s->run_cap ? s->run_cap * 2 : 16;
        hp_sort_run_t *runs = (hp_sort_run_t *)realloc(s->runs, cap * sizeof(*runs));
        if (!runs) return HP_ERR_NOMEM;
        s->runs = runs;
        s->run_cap = cap;
    }
    s->runs[s->run_count++] = *run;
    s->run_bytes += run->bytes;
    return HP_OK;
}

/* ---- merging ------------------------------------------------------------------ */

/* The next record of a run being merged */
typedef struct cursor_s {
    hp_sort_run_t *run;
    size_t idx;            /* the run's place: equal keys keep input order */
    size_t pos;            /* next entry, for a run in memory */
    uint8_t *buf;          /* the current record, for a run on disk */
    size_t buf_cap;
    hp_sort_entry_t cur;
} cursor_t;

static hp_error_t cursor_next(cursor_t *c, bool *have) {
    hp_sort_run_t *run = c->run;
    if (!run->file) {
        *have = c->pos < run->count;
        if (*have) c->cur = run->entries[c->pos++];
        return HP_OK;
    }
    uint8_t head[8];
    size_t got = fread(head, 1, 8, run->file);
    if (got == 0 && feof(run->file)) {
        *have = false;
        return HP_OK;
    }
    if (got != 8) return HP_ERR_IO;
    size_t size = rec_size(head);
    if (size > c->buf_cap) {
        uint8_t *buf = (uint8_t *)realloc(c->buf, size);
        if (!buf) return HP_ERR_NOMEM;
        c->buf = buf;
        c->buf_cap = size;
    }
    memcpy(c->buf, head, 8);
    if (fread(c->buf + 8, 1, size - 8, run->file) != size - 8) return HP_ERR_IO;
    c->cur.rec = c->buf;
    set_prefix(&c->cur, rec_key(c->buf), rec_key_len(c->buf));
    *have = true;
    return HP_OK;
}

HP_INLINE bool cursor_less(const cursor_t *a, const cursor_t *b, bool long_keys) {
    int c = entry_cmp(&a->cur, &b->cur, long_keys);
    return c != 0 ? c < 0 : a->idx < b->idx;
}

static void cursor_down(cursor_t *h, size_t n, size_t i, bool long_keys) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && cursor_less(&h[l], &h[m], long_keys)) m = l;
        if (r < n && cursor_less(&h[r], &h[m], long_keys)) m = r;
        if (m == i) return;
        cursor_t t = h[m];
        h[m] = h[i];
        h[i] = t;
        i = m;
    }
}

typedef hp_error_t (*take_fn)(void *ctx, const uint8_t *rec);

/* Merges runs [first, last) in order and hands each record to take, at
 * most limit of them */
static hp_error_t merge_runs(hp_sorter_t *s, size_t first, size_t last, uint64_t limit,
                             take_fn take, void *ctx, uint64_t *taken) {
    *taken = 0;
    cursor_t *heap = (cursor_t *)calloc(last > first ? last - first : 1, sizeof(*heap));
    if (!heap) return HP_ERR_NOMEM;
    hp_error_t err = HP_OK;
    size_t n = 0;
    for (size_t i = first; err == HP_OK && i < last; i++) {
        bool have = false;
        heap[n].run = &s->runs[i];
        heap[n].idx = i;
        err = cursor_next(&heap[n], &have);
        if (have) {
            n++;
        } else {
            free(heap[n].buf);
            heap[n].buf = NULL;
        }
    }
    for (size_t i = n / 2; i-- > 0;) cursor_down(heap, n, i, s->long_keys);

    while (err == HP_OK && n > 0 && *taken < limit) {
        if ((err = take(ctx, heap[0].cur.rec)) != HP_OK) break;
        (*taken)++;
        bool have = false;
        err = cursor_next(&heap[0], &have);
        if (!have) {
            free(heap[0].buf);
            heap[0] = heap[--n];
        }
        cursor_down(heap, n, 0, s->long_keys);
    }
    for (size_t i = 0; i < n; i++) free(heap[i].buf);
    free(heap);
    return err;
}

static FILE *open_run_file(void) {
    FILE *f = tmpfile();
    if (f) setvbuf(f, NULL, _IOFBF, 1 << 20);
    return f;
}

static hp_error_t write_rec(void *ctx, const uint8_t *rec) {
    return fwrite(rec, 1, rec_size(rec), (FILE *)ctx) == rec_size(rec) ? HP_OK : HP_ERR_IO;
}

static hp_error_t close_run_file(FILE *f, hp_error_t err) {
    if (err == HP_OK && fflush(f) != 0) err = HP_ERR_IO;
    if (err != HP_OK) {
        fclose(f);
        return err;
    }
    rewind(f);
    return HP_OK;
}

/* Writes a run held in memory to a temporary file and frees its memory */
static hp_error_t run_to_disk(hp_sorter_t *s, hp_sort_run_t *run) {
    FILE *f = open_run_file();
    if (!f) return HP_ERR_IO;
    hp_error_t err = HP_OK;
    for (size_t i = 0; err == HP_OK && i < run->count; i++) err = write_rec(f, run->entries[i].rec);
    if ((err = close_run_file(f, err)) != HP_OK) return err;
    s->run_bytes -= run->bytes;
    free_run(run);
    run->file = f;
    return HP_OK;
}

#define MERGE_FAN_IN 16

/* Sixteen neighbouring runs on disk of one level merge into one run of the
 * next level, so a row is rewritten a logarithmic number of times and few
 * files are open at once, however large the input */
static hp_error_t compact_runs(hp_sorter_t *s) {
    size_t i = 0;
    while (i + MERGE_FAN_IN <= s->run_count) {
        size_t j = i;
        while (j < i + MERGE_FAN_IN && s->runs[j].file && s->runs[j].level == s->runs[i].level) j++;
        if (j < i + MERGE_FAN_IN) {
            i = j > i ? j : i + 1;
            continue;
        }
        FILE *f = open_run_file();
        if (!f) return HP_ERR_IO;
        uint64_t n = 0;
        hp_error_t err = merge_runs(s, i, j, UINT64_MAX, write_rec, f, &n);
        if ((err = close_run_file(f, err)) != HP_OK) return err;
        int level = s->runs[i].level + 1;
        for (size_t k = i; k < j; k++) free_run(&s->runs[k]);
        s->runs[i] = (hp_sort_run_t){ .file = f, .level = level };
        memmove(&s->runs[i + 1], &s->runs[j], (s->run_count - j) * sizeof(*s->runs));
        s->run_count -= MERGE_FAN_IN - 1;
        i = 0;
    }
    return HP_OK;
}

hp_error_t hp_sorter_finish(hp_sorter_t *s) {
    if (s->count == 0) return HP_OK;
    hp_error_t err = sort_entries(s->entries, s->count, s->long_keys);
    if (err != HP_OK) return err;
    hp_sort_run_t run = {
        .entries = s->entries, .count = s->count, .blocks = s->blocks,
        .own_recs = s->limit != 0, .bytes = s->held,
    };
    if ((err = push_run(s, &run)) != HP_OK) return err;
    s->entries = NULL;
    s->blocks = NULL;
    s->count = s->cap = s->held = 0;
    return HP_OK;
}

/* Keeps the rows held in memory within the budget */
static hp_error_t enforce_budget(hp_sorter_t *s) {
    for (size_t i = 0; i < s->run_count && s->run_bytes > s->budget; i++) {
        if (!s->runs[i].file) {
            hp_error_t err = run_to_disk(s, &s->runs[i]);
            if (err != HP_OK) return err;
        }
    }
    return compact_runs(s);
}

/* ---- public ------------------------------------------------------------------- */

hp_error_t hp_sorter_init(hp_sorter_t *s, const char *spec, uint64_t limit, size_t budget) {
    if (!s || !spec) return HP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    s->limit = limit;
    s->budget = budget ? budget : HP_SORT_DEFAULT_BUDGET;

    const char *p = spec;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        bool desc = (*p == '-');
        if (desc) p++;
        while (*p && isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t len = (size_t)(end - start);
        if (len == 0 || len >= sizeof(s->keys[0].name) || s->key_count == HP_SORT_MAX_KEYS) {
            return HP_ERR_INVALID_ARG;
        }
        hp_sort_key_t *key = &s->keys[s->key_count++];
        memcpy(key->name, start, len);
        key->name[len] = '\0';
        key->col_index = -1;
        key->desc = desc;
        if (!*p) return HP_OK;
        p++;
    }
}

void hp_sorter_free(hp_sorter_t *s) {
    if (!s) return;
    hp_sort_run_t pending = {
        .entries = s->entries, .count = s->count, .blocks = s->blocks, .own_recs = s->limit != 0,
    };
    free_run(&pending);
    for (size_t i = 0; i < s->run_count; i++) free_run(&s->runs[i]);
    free(s->runs);
    free(s->key);
    memset(s, 0, sizeof(*s));
}

hp_error_t hp_sorter_bind(hp_sorter_t *s, const hp_header_t *header, const char **missing) {
    for (size_t k = 0; k < s->key_count; k++) {
        s->keys[k].col_index = hp_resolve_column(header, s->keys[k].name);
        if (s->keys[k].col_index < 0) {
            *missing = s->keys[k].name;
            return HP_ERR_COLUMN_NOT_FOUND;
        }
    }
    return HP_OK;
}

static hp_error_t grow_entries(hp_sorter_t *s) {
    size_t cap = s->cap ? s->cap * 2 : 1024;
    hp_sort_entry_t *e = (hp_sort_entry_t *)realloc(s->entries, cap * sizeof(*e));
    if (!e) return HP_ERR_NOMEM;
    s->entries = e;
    s->cap = cap;
    return HP_OK;
}

static uint8_t *block_alloc(hp_sorter_t *s, size_t size) {
    hp_sort_block_t *b = s->blocks;
    if (!b || b->cap - b->used < size) {
        size_t cap = size > BLOCK_SIZE ? size : BLOCK_SIZE;
        b = (hp_sort_block_t *)malloc(sizeof(*b) + cap);
        if (!b) return NULL;
        b->next = s->blocks;
        b->used = 0;
        b->cap = cap;
        s->blocks = b;
    }
    uint8_t *p = b->data + b->used;
    b->used += size;
    return p;
}

hp_error_t hp_sorter_add(hp_sorter_t *s, const hp_row_t *row, const void *line, size_t len) {
    size_t key_len = 0;
    hp_error_t err = build_key(s, row, &key_len);
    if (err != HP_OK) return err;
    hp_sort_entry_t e;
    set_prefix(&e, s->key, key_len);
    const size_t size = 8 + key_len + len;

    if (s->limit) {
        /* A full heap only takes a row that sorts before its largest; most
         * rows end here, after one comparison and no allocation */
        if (s->count == s->limit) {
            const hp_sort_entry_t *top = &s->entries[0];
            int c = prefix_cmp(&e, top);
            if (c == 0) c = tail_cmp(s->key, key_len, rec_key(top->rec), rec_key_len(top->rec));
            if (c >= 0) return HP_OK;
            if (!(e.rec = (uint8_t *)malloc(size))) return HP_ERR_NOMEM;
            put_rec(e.rec, s->key, key_len, line, len);
            s->held += size - rec_size(s->entries[0].rec);
            free(s->entries[0].rec);
            s->entries[0] = e;
            heap_down(s->entries, s->count, 0);
            return HP_OK;
        }
        if (s->count == s->cap && (err = grow_entries(s)) != HP_OK) return err;
        if (!(e.rec = (uint8_t *)malloc(size))) return HP_ERR_NOMEM;
        put_rec(e.rec, s->key, key_len, line, len);
        s->entries[s->count++] = e;
        s->held += size;
        heap_up(s->entries, s->count - 1);
        return HP_OK;
    }

    if (s->count == s->cap && (err = grow_entries(s)) != HP_OK) return err;
    if (!(e.rec = block_alloc(s, size))) return HP_ERR_NOMEM;
    put_rec(e.rec, s->key, key_len, line, len);
    s->entries[s->count++] = e;
    s->held += size + sizeof(e);
    if (s->held + s->run_bytes > s->budget) {
        if ((err = hp_sorter_finish(s)) != HP_OK) return err;
        return enforce_budget(s);
    }
    return HP_OK;
}

hp_error_t hp_sorter_absorb(hp_sorter_t *dst, hp_sorter_t *src) {
    hp_error_t err = hp_sorter_finish(src);
    dst->long_keys |= src->long_keys;
    for (size_t i = 0; err == HP_OK && i < src->run_count; i++) {
        if ((err = push_run(dst, &src->runs[i])) == HP_OK) memset(&src->runs[i], 0, sizeof(src->runs[i]));
    }
    if (err != HP_OK) return err;
    src->run_count = 0;
    src->run_bytes = 0;
    return dst->limit ? HP_OK : enforce_budget(dst);
}

/* ---- parallel merge ----------------------------------------------------------- */

/* Runs held in memory merge in parallel: sampled keys cut the output into
 * parts, each part is a key range of every run, and threads merge parts
 * while this thread writes the finished ones in order. At most `window`
 * parts are in flight, so the extra memory stays a few parts' worth. */

typedef struct merge_part_s {
    uint8_t *out;
    size_t len, cap;
    bool done;
    hp_error_t err;
} merge_part_t;

typedef struct merge_pool_s {
    hp_sorter_t *s;
    size_t runs, parts, window;
    size_t *bounds;        /* bounds[q * runs + i]: where part q starts in run i */
    merge_part_t *slot;    /* part q uses slot[q % window] */
    size_t next, written;
    bool stop;
    hp_mutex_t mu;
    hp_cond_t progress;
} merge_pool_t;

/* A place in the merged order: a run's entry, ties broken by run then
 * position, as the serial merge breaks them */
typedef struct merge_ref_s {
    const hp_sort_entry_t *e;
    size_t run, pos;
} merge_ref_t;

static int ref_cmp(const merge_ref_t *a, const merge_ref_t *b) {
    int c = entry_cmp(a->e, b->e, true);
    if (c != 0) return c;
    if (a->run != b->run) return a->run < b->run ? -1 : 1;
    return (a->pos > b->pos) - (a->pos < b->pos);
}

static int ref_qsort_cmp(const void *a, const void *b) {
    return ref_cmp((const merge_ref_t *)a, (const merge_ref_t *)b);
}

/* How many entries of run i come before the splitter */
static size_t run_lower_bound(const hp_sort_run_t *run, size_t i, const merge_ref_t *split) {
    size_t lo = 0, hi = run->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        merge_ref_t here = { &run->entries[mid], i, mid };
        if (ref_cmp(&here, split) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static hp_error_t part_append(merge_part_t *p, const void *data, size_t len) {
    if (p->len + len > p->cap) {
        size_t cap = p->cap ? p->cap : 1 << 20;
        while (cap < p->len + len) cap *= 2;
        uint8_t *out = (uint8_t *)realloc(p->out, cap);
        if (!out) return HP_ERR_NOMEM;
        p->out = out;
        p->cap = cap;
    }
    memcpy(p->out + p->len, data, len);
    p->len += len;
    return HP_OK;
}

typedef struct part_cursor_s {
    const hp_sort_entry_t *e, *end;
    size_t run;
} part_cursor_t;

HP_INLINE bool part_cursor_less(const part_cursor_t *a, const part_cursor_t *b, bool long_keys) {
    int c = entry_cmp(a->e, b->e, long_keys);
    return c != 0 ? c < 0 : a->run < b->run;
}

static void part_cursor_down(part_cursor_t *h, size_t n, size_t i, bool long_keys) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && part_cursor_less(&h[l], &h[m], long_keys)) m = l;
        if (r < n && part_cursor_less(&h[r], &h[m], long_keys)) m = r;
        if (m == i) return;
        part_cursor_t t = h[m];
        h[m] = h[i];
        h[i] = t;
        i = m;
    }
}

static hp_error_t merge_part(merge_pool_t *pool, size_t q, merge_part_t *out) {
    const hp_sorter_t *s = pool->s;
    part_cursor_t *heap = (part_cursor_t *)malloc(pool->runs * sizeof(*heap));
    if (!heap) return HP_ERR_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < pool->runs; i++) {
        size_t lo = pool->bounds[q * pool->runs + i], hi = pool->bounds[(q + 1) * pool->runs + i];
        if (lo < hi) heap[n++] = (part_cursor_t){ &s->runs[i].entries[lo], &s->runs[i].entries[hi], i };
    }
    for (size_t i = n / 2; i-- > 0;) part_cursor_down(heap, n, i, s->long_keys);

    hp_error_t err = HP_OK;
    out->len = 0;
    while (err == HP_OK && n > 0) {
        const uint8_t *rec = heap[0].e->rec;
        err = part_append(out, rec_line(rec), rec_line_len(rec));
        if (++heap[0].e == heap[0].end) heap[0] = heap[--n];
        part_cursor_down(heap, n, 0, s->long_keys);
    }
    free(heap);
    return err;
}

static void *merge_main(void *arg) {
    merge_pool_t *pool = (merge_pool_t *)arg;
    hp_mutex_lock(&pool->mu);
    for (;;) {
        while (!pool->stop && pool->next < pool->parts && pool->next >= pool->written + pool->window) {
            hp_cond_wait(&pool->progress, &pool->mu);
        }
        if (pool->stop || pool->next >= pool->parts) break;
        size_t q = pool->next++;
        merge_part_t *slot = &pool->slot[q % pool->window];
        hp_mutex_unlock(&pool->mu);

        hp_error_t err = merge_part(pool, q, slot);

        hp_mutex_lock(&pool->mu);
        slot->err = err;
        slot->done = true;
        hp_cond_broadcast(&pool->progress);
    }
    hp_mutex_unlock(&pool->mu);
    return NULL;
}

/* Cuts the runs into parts of about equal size, from sampled keys */
static hp_error_t plan_parts(merge_pool_t *pool, size_t total) {
    const hp_sorter_t *s = pool->s;
    const size_t per_run = 4 * pool->parts;
    merge_ref_t *samples = (merge_ref_t *)malloc(pool->runs * per_run * sizeof(*samples));
    pool->bounds = (size_t *)calloc((pool->parts + 1) * pool->runs, sizeof(size_t));
    if (!samples || !pool->bounds) {
        free(samples);
        return HP_ERR_NOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < pool->runs; i++) {
        const hp_sort_run_t *run = &s->runs[i];
        /* Samples in proportion to the run's size keep the parts even */
        size_t k = (size_t)((double)per_run * pool->runs * run->count / (double)total);
        if (k > run->count) k = run->count;
        if (k > per_run) k = per_run;
        for (size_t j = 0; j < k; j++) {
            size_t pos = (size_t)((double)(j + 1) * run->count / (double)(k + 1));
            samples[n++] = (merge_ref_t){ &run->entries[pos], i, pos };
        }
    }
    qsort(samples, n, sizeof(*samples), ref_qsort_cmp);

    for (size_t i = 0; i < pool->runs; i++) pool->bounds[pool->parts * pool->runs + i] = s->runs[i].count;
    for (size_t q = 1; q < pool->parts; q++) {
        if (n == 0) break;
        const merge_ref_t *split = &samples[q * n / pool->parts];
        for (size_t i = 0; i < pool->runs; i++) {
            pool->bounds[q * pool->runs + i] = run_lower_bound(&s->runs[i], i, split);
        }
    }
    if (n == 0) {
        /* Too few rows to sample: one part takes everything */
        for (size_t q = 1; q < pool->parts; q++) {
            for (size_t i = 0; i < pool->runs; i++) pool->bounds[q * pool->runs + i] = 0;
        }
    }
    free(samples);
    return HP_OK;
}

static hp_error_t merge_parallel(hp_sorter_t *s, void (*emit)(void *ctx, const void *data, size_t len),
                                 void *ctx, uint64_t *written) {
    size_t total = 0;
    for (size_t i = 0; i < s->run_count; i++) total += s->runs[i].count;

    merge_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.s = s;
    pool.runs = s->run_count;
    pool.parts = (size_t)s->threads * 8;
    pool.window = (size_t)s->threads + 2;
    pool.slot = (merge_part_t *)calloc(pool.window, sizeof(*pool.slot));
    hp_error_t err = pool.slot ? plan_parts(&pool, total) : HP_ERR_NOMEM;
    hp_mutex_init(&pool.mu);
    hp_cond_init(&pool.progress);

    hp_thread_t threads[64];
    int started = 0;
    for (int t = 0; err == HP_OK && t < s->threads && t < 64; t++) {
        if (hp_thread_create(&threads[t], merge_main, &pool)) started++;
    }
    if (err == HP_OK && started == 0) err = HP_ERR_NOMEM;

    for (size_t q = 0; err == HP_OK && q < pool.parts; q++) {
        merge_part_t *slot = &pool.slot[q % pool.window];
        hp_mutex_lock(&pool.mu);
        while (!slot->done) hp_cond_wait(&pool.progress, &pool.mu);
        hp_mutex_unlock(&pool.mu);
        if ((err = slot->err) == HP_OK && slot->len) emit(ctx, slot->out, slot->len);

        hp_mutex_lock(&pool.mu);
        slot->done = false;
        pool.written = q + 1;
        hp_cond_broadcast(&pool.progress);
        hp_mutex_unlock(&pool.mu);
    }

    hp_mutex_lock(&pool.mu);
    pool.stop = true;
    hp_cond_broadcast(&pool.progress);
    hp_mutex_unlock(&pool.mu);
    for (int t = 0; t < started; t++) hp_thread_join(threads[t]);

    for (size_t w = 0; pool.slot && w < pool.window; w++) free(pool.slot[w].out);
    free(pool.slot);
    free(pool.bounds);
    hp_cond_destroy(&pool.progress);
    hp_mutex_destroy(&pool.mu);
    if (err == HP_OK) *written = total;
    return err;
}

/* ---- the last run ------------------------------------------------------------- */

/* At the end, with threads to spare, the rows still held sort as segments
 * on several threads; the segments, in input order, then merge like runs.
 * Only the last step may do this: the segments share the first one's
 * memory, so none may be written to disk or freed on its own. */

typedef struct segment_job_s {
    hp_sort_entry_t *entries;
    size_t count;
    bool long_keys;
    hp_error_t err;
} segment_job_t;

static void *segment_main(void *arg) {
    segment_job_t *job = (segment_job_t *)arg;
    job->err = sort_entries(job->entries, job->count, job->long_keys);
    return NULL;
}

#define SEGMENT_MIN 65536

static hp_error_t finish_in_segments(hp_sorter_t *s) {
    size_t n = (size_t)s->threads;
    if (n > 64) n = 64;
    if (s->count / n < SEGMENT_MIN) n = s->count / SEGMENT_MIN;
    if (n < 2 || s->limit) return hp_sorter_finish(s);

    segment_job_t jobs[64];
    hp_thread_t threads[64];
    bool started[64] = { false };
    const size_t per = (s->count + n - 1) / n;
    for (size_t k = 0; k < n; k++) {
        size_t lo = k * per, hi = lo + per < s->count ? lo + per : s->count;
        jobs[k] = (segment_job_t){ s->entries + lo, hi - lo, s->long_keys, HP_OK };
        if (k > 0) started[k] = hp_thread_create(&threads[k], segment_main, &jobs[k]);
    }
    for (size_t k = 0; k < n; k++) {
        if (k == 0 || !started[k]) segment_main(&jobs[k]);
    }
    hp_error_t err = HP_OK;
    for (size_t k = 0; k < n; k++) {
        if (started[k]) hp_thread_join(threads[k]);
        if (jobs[k].err != HP_OK) err = jobs[k].err;
    }

    for (size_t k = 0; err == HP_OK && k < n; k++) {
        hp_sort_run_t run = {
            .entries = jobs[k].entries, .count = jobs[k].count, .borrowed = k > 0,
            .blocks = k == 0 ? s->blocks : NULL, .bytes = k == 0 ? s->held : 0,
        };
        err = push_run(s, &run);
        if (err == HP_OK && k == 0) {
            s->entries = NULL;
            s->blocks = NULL;
        }
    }
    s->count = s->cap = s->held = 0;
    return err;
}

struct line_sink {
    void (*emit)(void *ctx, const void *data, size_t len);
    void *ctx;
};

static hp_error_t take_line(void *ctx, const uint8_t *rec) {
    struct line_sink *sink = (struct line_sink *)ctx;
    sink->emit(sink->ctx, rec_line(rec), rec_line_len(rec));
    return HP_OK;
}

hp_error_t hp_sorter_write(hp_sorter_t *s, void (*emit)(void *ctx, const void *data, size_t len),
                           void *ctx, uint64_t *written) {
    *written = 0;
    hp_error_t err = s->threads > 1 ? finish_in_segments(s) : hp_sorter_finish(s);
    if (err != HP_OK) return err;
    const uint64_t limit = s->limit ? s->limit : UINT64_MAX;

    /* Without a limit, runs held in memory merge on several threads */
    bool in_memory = true;
    for (size_t i = 0; i < s->run_count; i++) in_memory &= s->runs[i].file == NULL;
    if (in_memory && s->run_count > 1 && s->threads > 1 && !s->limit) {
        return merge_parallel(s, emit, ctx, written);
    }

    /* One run in memory needs no merge */
    if (s->run_count == 1 && !s->runs[0].file) {
        const hp_sort_run_t *run = &s->runs[0];
        for (size_t i = 0; i < run->count && *written < limit; i++, (*written)++) {
            emit(ctx, rec_line(run->entries[i].rec), rec_line_len(run->entries[i].rec));
        }
        return HP_OK;
    }

    struct line_sink sink = { emit, ctx };
    return merge_runs(s, 0, s->run_count, limit, take_line, &sink, written);
}
