/* web.c -- a browser, in text
 *
 * Every browser anyone has used draws pictures. This one has eighty
 * columns, sixteen colours and no mouse pointer, which is what PicoOS has
 * to offer, and it turns out that is very nearly enough: what a web page
 * is, underneath the pictures, is a document. Headings, paragraphs, lists,
 * and sentences with other sentences hiding inside them. Strip the markup
 * and lay the words out in a column and you have most of the web.
 *
 * Which is not to say the markup can simply be thrown away. The part that
 * matters is the anchor: a stretch of text that is also a door. So the
 * parser here keeps exactly one thing from the tag soup -- where the doors
 * are -- numbers them in the order they appear, and lets you type a number
 * to walk through one.
 *
 * The rendering is done by walking the page from the beginning every time
 * something changes. That sounds wasteful and on a machine this size it is
 * the opposite: keeping a folded copy of the page in memory would cost
 * more than the page, and walking thirty thousand bytes in a straight line
 * is work a 486 does without noticing. Nothing is stored but the page
 * itself and the number of the line you are looking at.
 *
 * The kernel speaks TLS 1.2 for it, and reports the reason when a server
 * will not talk. Encrypting a connection means carrying a certificate
 * authority, a handful of ciphers and a good deal of arithmetic, and this
 * kernel does not have them yet -- so an encrypted page is reported as
 * what it is rather than pretended into working.
 */

#include "picoapp.h"

#define WMAX   132             /* widest console this will ever render for */
#define UMAX   192             /* longest address we will hold            */
#define HIST   24              /* pages we remember                        */

/* what a character on the screen is */
#define A_PLAIN   0
#define A_LINK    1
#define A_HEAD    2
#define A_STRONG  3
#define A_PRE     4
#define A_QUOTE   5

/* what a walk over the page is being asked to do */
#define W_PAGE    0            /* fold it into lines and draw some of them */
#define W_LINKS   1            /* list every door, numbered                */
#define W_PICK    2            /* find door number n and nothing else      */

static char *page;              /* the html, exactly as it arrived */
static u32   pagesize;
static u32   plen;              /* how much of it is a page         */
static char  url[UMAX];         /* the one on screen                */
static char  title[96];
static char  status[48];        /* what the server said             */
static char  msg[96];           /* what we want to say to you       */
static int   top;               /* first line on screen             */
static int   nlines;            /* how many lines the page folds into */
static int   nlinks;            /* how many doors it has            */
static int   showing_links;     /* 1 = the list of doors, not the page */
static int   rows;              /* lines of page on screen          */
static int   wide;

static char  hist[HIST][UMAX];
static int   nhist, hpos;

static char  input[UMAX];
static int   inlen;
static int   inputting;

/* ---------------------------------------------------------------- */
/*  the handful of string things the api does not hand out           */
/* ---------------------------------------------------------------- */

static void *mem_set(void *d, int c, unsigned n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static void copy_str(char *d, const char *s, int max)
{
    int n = 0;
    if (max <= 0) return;
    for (; s[n] && n < max - 1; n++) d[n] = s[n];
    d[n] = 0;
}

static void cat_str(char *d, const char *s, int max)
{
    int n = 0;
    while (d[n] && n < max) n++;
    for (int i = 0; s[i] && n < max - 1; i++) d[n++] = s[i];
    d[n] = 0;
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int str_ncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/* ---------------------------------------------------------------- */
/*  folding the page into lines                                      */
/* ---------------------------------------------------------------- */

typedef struct {
    int  mode;
    int  first, count;          /* which lines to draw                  */
    int  w;
    int  line, col;
    char lb[WMAX + 4];
    u8   la[WMAX + 4];
    u16  ll[WMAX + 4];
    int  nlink;                 /* doors passed so far               */
    int  attr;                  /* what to write in next             */
    int  in_pre, in_title;
    int  curlink;               /* the door we are standing in       */
    int  want;                  /* W_PICK: which door we are after   */
    char *out; int outmax;      /* W_PICK: where its address goes    */
    char ltext[80];             /* W_LINKS: the words inside a door  */
    int  got_it;
    int  out_on;                /* W_LINKS: we are drawing a line on purpose */
    int  blank;                 /* the line we just ended was an empty one */
} walk_t;

static walk_t W;                /* one of them, reused, never recursed into */

static void flush_line(walk_t *w)
{
    if (w->col == 0) {
        if (w->mode == W_LINKS) return;      /* no blank lines in a list */
        if (w->line == 0 || w->blank) return; /* nor two in a row, nor one first */
        w->blank = 1;
    } else {
        w->blank = 0;
    }

    if (w->count > 0 && w->line >= w->first && w->line < w->first + w->count) {
        api->gotoxy(0, 1 + w->line - w->first);
        int i = 0;
        while (i < w->col) {
            u8 r = w->la[i];
            int j = i;
            while (j < w->col && w->la[j] == r) j++;
            switch (r) {
            case A_LINK:   api->setcolor(YELLOW, BLACK); break;
            case A_HEAD:   api->setcolor(WHITE,  BLACK); break;
            case A_STRONG: api->setcolor(WHITE,  BLACK); break;
            case A_PRE:    api->setcolor(LCYAN,  BLACK); break;
            case A_QUOTE:  api->setcolor(DGREY,  BLACK); break;
            default:       api->setcolor(LGREY,  BLACK); break;
            }
            for (int k = i; k < j; k++) api->putc(w->lb[k]);
            i = j;
        }
        for (int k = w->col; k < w->w; k++) api->putc(' ');
        api->setcolor(LGREY, BLACK);
    }
    w->line++;
    w->col = 0;
}

static void put_char(walk_t *w, char c, u8 a, u16 link)
{
    /* In the list of doors nothing is written but the doors themselves:
     * paragraphs, bullets and preformatted blocks are all furniture there. */
    if (w->mode == W_LINKS && !w->out_on) return;
    if (w->col >= w->w) flush_line(w);
    w->lb[w->col] = c;
    w->la[w->col] = a;
    w->ll[w->col] = link;
    w->col++;
}

static void put_str(walk_t *w, const char *s, u8 a, u16 link)
{
    for (; *s; s++) {
        if (*s == '\n') { flush_line(w); continue; }
        put_char(w, *s, a, link);
    }
}

/* One word, wrapped rather than split. */
static void put_word(walk_t *w, const char *s, int len, u8 a, u16 link)
{
    if (len > w->w) len = w->w;
    if (w->col && w->col + 1 + len > w->w) flush_line(w);
    if (w->col) put_char(w, ' ', A_PLAIN, 0);
    for (int i = 0; i < len; i++) put_char(w, s[i], a, link);
}

/* "&amp;" and friends. Returns the character and, through adv, how many
 * bytes of the page it ate. */
static char entity(const char *s, int left, int *adv)
{
    *adv = 0;
    if (left < 3 || s[0] != '&') return 0;
    if (s[1] == '#') {
        int v = 0, i = 2, n = 0;
        while (i < left && s[i] >= '0' && s[i] <= '9' && n < 5) {
            v = v * 10 + (s[i] - '0'); i++; n++;
        }
        if (i < left && s[i] == ';' && n) {
            *adv = i + 1;
            return (char)(v & 0x7F);
        }
        return 0;
    }
    static const struct { const char *n; char c; } ent[] = {
        { "amp;",  '&' },  { "lt;",   '<' },  { "gt;",   '>' },
        { "quot;", '"' },  { "apos;", '\'' }, { "nbsp;", ' ' },
        { "copy;", 'c' },  { "hellip;", '.' },{ "mdash;", '-' },
        { "ndash;", '-' }, { "rsquo;", '\'' },{ "lsquo;", '\'' },
        { "ldquo;", '"' }, { "rdquo;", '"' }, { "laquo;", '<' },
        { "raquo;", '>' }, { "times;", 'x' }, { "shy;",  0 },
        { "nbsp",  ' ' },  { "amp",   '&' },  { "lt",    '<' },
        { "gt",    '>' },  { "quot",  '"' },
    };
    for (unsigned i = 0; i < sizeof ent / sizeof ent[0]; i++) {
        int l = 0;
        while (ent[i].n[l]) l++;
        if (left >= l + 1 && !str_ncmp(s + 1, ent[i].n, l)) {
            *adv = l + 1;
            return ent[i].c;
        }
    }
    return 0;
}

/* A run of ordinary text between two tags. */
static void w_text(walk_t *w, const char *s, int len)
{
    if (w->in_title) {
        int n = 0;
        while (title[n]) n++;
        for (int k = 0; k < len && n < (int)sizeof title - 1; k++) {
            int adv; char e = entity(s + k, len - k, &adv);
            if (e) { title[n++] = e; k += adv - 1; }
            else if (!is_ws(s[k]) || (n && title[n - 1] != ' ')) title[n++] = s[k];
        }
        title[n] = 0;
        return;
    }

    if (w->in_pre) {
        for (int k = 0; k < len; k++) {
            int adv; char e = entity(s + k, len - k, &adv);
            if (e) { put_char(w, e, A_PRE, 0); k += adv - 1; }
            else if (s[k] == '\n') flush_line(w);
            else if (s[k] == '\r') continue;
            else put_char(w, s[k], A_PRE, 0);
        }
        return;
    }

    int i = 0;
    while (i < len) {
        while (i < len && is_ws(s[i])) i++;
        int st = i;
        while (i < len && !is_ws(s[i])) i++;
        if (i <= st) continue;

        char word[WMAX];
        int wn = 0;
        for (int k = st; k < i && wn < WMAX - 2; k++) {
            int adv; char e = entity(s + k, i - k, &adv);
            if (e) { word[wn++] = e; k += adv - 1; }
            else word[wn++] = s[k];
        }
        if (!wn) continue;
        word[wn] = 0;

        /* inside a door: the words name it, whether we list it or draw it */
        if (w->curlink) {
            int n = 0;
            while (w->ltext[n]) n++;
            if (n && n < 70) w->ltext[n++] = ' ';
            for (int k = 0; k < wn && n < 70; k++) w->ltext[n++] = word[k];
            w->ltext[n] = 0;
        }
        if (w->mode == W_PAGE)
            put_word(w, word, wn, (u8)w->attr, (u16)w->curlink);
    }
}

/* ---- tags ---- */

static int tag_end(const char *s, int len)
{
    for (int i = 1; i < len; i++)
        if (s[i] == '>') return i;
    return len - 1;
}

/* Copy the value of name="..." out of a tag. */
static int tag_attr(const char *s, int len, const char *name,
                    char *out, int max)
{
    int nl = 0;
    while (name[nl]) nl++;
    out[0] = 0;
    if (max <= 0) return 0;

    for (int i = 0; i + nl + 1 < len; i++) {
        if (s[i] != name[0]) continue;
        if (str_ncmp(s + i, name, nl)) continue;
        int j = i + nl;
        while (j < len && (s[j] == ' ' || s[j] == '=')) j++;
        if (j >= len) return 0;
        char q = s[j];
        int end;
        if (q == '"' || q == '\'') {
            j++;
            end = j;
            while (end < len && s[end] != q) end++;
        } else {
            end = j;
            while (end < len && !is_ws(s[end]) && s[end] != '>') end++;
        }
        int n = 0;
        for (int k = j; k < end && n < max - 1; k++) {
            int adv; char e = entity(s + k, end - k, &adv);
            if (e) { out[n++] = e; k += adv - 1; } else out[n++] = s[k];
        }
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

static void tag_name(const char *s, int len, char *out, int max)
{
    int i = 0, n = 0;
    if (s[i] == '/') i++;
    while (i < len && n < max - 1 && !is_ws(s[i]) && s[i] != '>' && s[i] != '/')
        out[n++] = s[i++];
    out[n] = 0;
    for (int k = 0; out[k]; k++)
        if (out[k] >= 'A' && out[k] <= 'Z') out[k] = (char)(out[k] + 32);
}

/* Is this tag one of the ones that ends the line it is standing on? */
static int is_block(const char *n)
{
    static const char *b[] = { "p", "div", "br", "li", "tr", "h1", "h2",
                               "h3", "h4", "h5", "h6", "hr", "table", "td",
                               "th", "ul", "ol", "dl", "dt", "dd", "pre",
                               "blockquote", "form", "center", "header",
                               "footer", "nav", "section", "article",
                               "main", "aside", "fieldset", "option",
                               "select", "textarea", "button", "head",
                               "body", "html", "figure", "figcaption",
                               "address", "summary", "details", 0 };
    for (int i = 0; b[i]; i++) if (!str_cmp(n, b[i])) return 1;
    return 0;
}

/* Walk the page once. Depending on the mode it draws it, lists its doors,
 * or goes looking for one particular door. Returns the number of lines the
 * page folds into. */
static int walk_page(walk_t *w)
{
    int i = 0;
    char href[UMAX];
    char name[16];

    w->line = 0; w->col = 0; w->nlink = 0; w->curlink = 0;
    w->in_pre = 0; w->in_title = 0; w->attr = A_PLAIN;
    title[0] = 0;

    while (i < (int)plen) {
        if (page[i] != '<') {
            int st = i;
            while (i < (int)plen && page[i] != '<') i++;
            w_text(w, page + st, i - st);
            continue;
        }

        /* a comment or a doctype: nothing here is meant for a reader */
        if (page[i + 1] == '!') {
            int j = i + 2;
            if (page[j] == '-' && page[j + 1] == '-') {
                j += 2;
                while (j + 2 < (int)plen &&
                       !(page[j] == '-' && page[j + 1] == '-' && page[j + 2] == '>'))
                    j++;
                i = (j + 2 < (int)plen) ? j + 3 : (int)plen;
            } else {
                while (i < (int)plen && page[i] != '>') i++;
                i++;
            }
            continue;
        }
        if (page[i + 1] == '?') {
            while (i < (int)plen && page[i] != '>') i++;
            i++;
            continue;
        }

        int e = tag_end(page + i, (int)plen - i);
        int len = e;                        /* the tag without its < and > */
        const char *t = page + i + 1;
        int closing = (t[0] == '/');
        tag_name(t, len, name, sizeof name);

        i += e + 1;

        /* the two kinds of tag whose contents are not for reading aloud */
        if (!str_cmp(name, "script") || !str_cmp(name, "style")) {
            const char *want = name[1] == 't' ? "</style>" : "</script>";
            int j = i, l = 0;
            while (want[l]) l++;
            while (j + l <= (int)plen) {
                int k = 0;
                while (k < l && (page[j + k] | 32) == (want[k] | 32)) k++;
                if (k == l) { j += l; break; }
                j++;
            }
            i = j;
            continue;
        }

        if (closing) {
            if (!str_cmp(name, "a")) {
                if (w->mode == W_LINKS && w->curlink) {
                    char line[WMAX + 4];
                    int n = 0;
                    line[n++] = '[';
                    { char num[8]; app_itoa(w->curlink, num);
                      for (int k = 0; num[k]; k++) line[n++] = num[k]; }
                    line[n++] = ']';
                    line[n++] = ' ';
                    const char *txt = w->ltext[0] ? w->ltext : href;
                    for (int k = 0; txt[k] && n < WMAX - 1; k++) line[n++] = txt[k];
                    line[n] = 0;
                    w->col = 0;
                    w->out_on = 1;
                    put_str(w, line, A_LINK, (u16)w->curlink);
                    w->out_on = 0;
                    flush_line(w);
                }
                w->curlink = 0;
                w->attr = A_PLAIN;
            } else if (!str_cmp(name, "title")) {
                w->in_title = 0;
            } else if (!str_cmp(name, "pre")) {
                w->in_pre = 0;
                w->attr = A_PLAIN;
                flush_line(w);
            } else if (!str_cmp(name, "blockquote")) {
                w->attr = A_PLAIN;
                flush_line(w);
            } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' &&
                       !name[2]) {
                w->attr = A_PLAIN;
                flush_line(w);
            } else if (is_block(name)) {
                flush_line(w);
            }
            continue;
        }

        /* ---- opening tags ---- */

        if (!str_cmp(name, "a")) {
            href[0] = 0;
            tag_attr(t, len, "href", href, sizeof href);
            if (href[0] && href[0] != '#' &&
                str_ncmp(href, "mailto:", 7) &&
                str_ncmp(href, "javascript:", 11) &&
                str_ncmp(href, "tel:", 4)) {
                w->nlink++;
                w->curlink = w->nlink;
                w->ltext[0] = 0;
                if (w->mode == W_PICK && w->curlink == w->want) {
                    copy_str(w->out, href, w->outmax);
                    w->got_it = 1;
                    return w->nlink;
                }
                if (w->mode == W_PAGE) {
                    char mark[12];
                    int n = 0;
                    mark[n++] = '[';
                    { char num[8]; app_itoa(w->curlink, num);
                      for (int k = 0; num[k]; k++) mark[n++] = num[k]; }
                    mark[n++] = ']';
                    mark[n] = 0;
                    w->attr = A_LINK;
                    put_word(w, mark, n, A_LINK, (u16)w->curlink);
                }
            } else {
                w->curlink = 0;
            }
            continue;
        }

        if (!str_cmp(name, "title")) { w->in_title = 1; continue; }
        if (!str_cmp(name, "pre"))   { flush_line(w); w->in_pre = 1;
                                       w->attr = A_PRE; continue; }
        if (!str_cmp(name, "li"))    { flush_line(w);
                                       put_word(w, " *", 2, A_PLAIN, 0); continue; }
        if (!str_cmp(name, "dt"))    { flush_line(w); continue; }
        if (!str_cmp(name, "dd"))    { flush_line(w);
                                       put_word(w, "   ", 3, A_PLAIN, 0); continue; }
        if (!str_cmp(name, "hr")) {
            flush_line(w);
            for (int k = 0; k < w->w; k++) put_char(w, '-', A_QUOTE, 0);
            flush_line(w);
            continue;
        }
        if (!str_cmp(name, "img")) {
            char alt[48];
            alt[0] = 0;
            tag_attr(t, len, "alt", alt, sizeof alt);
            if (w->mode == W_PAGE) {
                if (alt[0]) {
                    w->attr = A_QUOTE;
                    put_word(w, "[", 1, A_QUOTE, (u16)w->curlink);
                    put_word(w, alt, app_strlen(alt), A_QUOTE, (u16)w->curlink);
                    put_word(w, "]", 1, A_QUOTE, (u16)w->curlink);
                    w->attr = A_PLAIN;
                } else {
                    put_word(w, "[image]", 7, A_QUOTE, (u16)w->curlink);
                }
            }
            continue;
        }
        if (!str_cmp(name, "br"))    { flush_line(w); continue; }
        if (!str_cmp(name, "meta") || !str_cmp(name, "link") ||
            !str_cmp(name, "input") || !str_cmp(name, "wbr") ||
            !str_cmp(name, "base") || !str_cmp(name, "area")) {
            continue;
        }
        if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
            flush_line(w);
            flush_line(w);
            w->attr = A_HEAD;
            continue;
        }
        if (!str_cmp(name, "blockquote")) { flush_line(w); w->attr = A_QUOTE; continue; }
        if (!str_cmp(name, "strong") || !str_cmp(name, "b") ||
            !str_cmp(name, "em") || !str_cmp(name, "i")) {
            if (w->attr == A_PLAIN) w->attr = A_STRONG;
            continue;
        }
        if (is_block(name)) { flush_line(w); continue; }
        /* anything else -- a span, a font, a table cell -- is furniture */
    }

    if (w->col) flush_line(w);
    return w->line;
}

/* How many lines does the page fold into? A walk that draws nothing is the
 * cheapest way to find out, and it also counts the doors. */
static int measure(void)
{
    mem_set(&W, 0, sizeof W);
    W.w = wide;
    W.mode = W_PAGE;
    W.count = 0;                /* draw nothing */
    nlinks = 0;
    nlines = walk_page(&W);
    nlinks = W.nlink;
    return nlines;
}

/* ---------------------------------------------------------------- */
/*  addresses                                                        */
/* ---------------------------------------------------------------- */

/* Turn what a page says -- "page.html", "/index", "//host/x" -- into a
 * whole address, using the address of the page it was found on. */
static void resolve(const char *base, const char *href, char *out, int max)
{
    static char head[UMAX];
    static char dir[UMAX];

    out[0] = 0;
    if (!href[0]) { copy_str(out, base, max); return; }

    if (!str_ncmp(href, "http://", 7) || !str_ncmp(href, "https://", 8)) {
        copy_str(out, href, max);
        return;
    }
    if (href[0] == '/' && href[1] == '/') {         /* protocol-relative */
        copy_str(out, "http:", max);
        cat_str(out, href, max);
        return;
    }

    /* the front of the address we came from: scheme://host[:port] */
    copy_str(head, base, sizeof head);
    if (str_ncmp(head, "http://", 7)) { copy_str(out, href, max); return; }
    {
        char *slash = head + 7;
        while (*slash && *slash != '/') slash++;
        *slash = 0;
    }

    if (href[0] == '/') {
        copy_str(out, head, max);
        cat_str(out, href, max);
        return;
    }

    /* everything else is relative to the directory the page was in */
    copy_str(dir, base + 7, sizeof dir);
    {
        int last = -1;
        for (int i = 0; dir[i]; i++) if (dir[i] == '/') last = i;
        if (last >= 0) dir[last + 1] = 0; else dir[0] = 0;
    }
    copy_str(out, head, max);
    cat_str(out, "/", max);
    cat_str(out, dir, max);
    cat_str(out, href, max);
}

/* Drop the #fragment: it means something to the page, not to the server. */
static void strip_frag(char *s)
{
    for (int i = 0; s[i]; i++)
        if (s[i] == '#') { s[i] = 0; return; }
}

/* ---------------------------------------------------------------- */
/*  the screen                                                       */
/* ---------------------------------------------------------------- */

static void bar(int row, const char *text, u8 fg, u8 bg)
{
    char line[WMAX + 4];
    int n = 0;

    line[n++] = ' ';
    for (int i = 0; text[i] && n < wide - 1; i++) line[n++] = text[i];
    while (n < wide) line[n++] = ' ';
    line[n] = 0;
    api->setcolor(fg, bg);
    api->gotoxy(0, row);
    api->puts(line);
    api->setcolor(LGREY, BLACK);
}

static void draw(void)
{
    int h = api->height();
    char line[WMAX + 4];
    int n = 0;

    wide = api->width();
    if (wide > WMAX) wide = WMAX;
    rows = h - 3;
    if (rows < 1) rows = 1;

    measure();

    if (top > nlines - rows) top = nlines - rows;
    if (top < 0) top = 0;

    api->clear();
    bar(0, title[0] ? title : url, WHITE, BLUE);

    /* what the server said, and where we are in the page */
    n = 0;
    if (status[0]) {
        for (int i = 0; status[i] && n < (int)sizeof line - 4; i++)
            line[n++] = status[i];
        line[n++] = ' '; line[n++] = '-'; line[n++] = ' ';
    }
    { char num[12];
      if (plen < 1024) { app_itoa((int)plen, num);
                         for (int i = 0; num[i] && n < (int)sizeof line - 6; i++)
                             line[n++] = num[i];
                         line[n++] = ' '; line[n++] = 'B'; }
      else { app_itoa((int)(plen / 1024), num);
             for (int i = 0; num[i] && n < (int)sizeof line - 6; i++)
                 line[n++] = num[i];
             line[n++] = 'K'; line[n++] = 'B'; }
      line[n++] = ' '; line[n++] = '-'; line[n++] = ' '; }
    { char num[12];
      app_itoa(nlinks, num);
      for (int i = 0; num[i] && n < (int)sizeof line - 4; i++) line[n++] = num[i];
      line[n++] = ' '; }
    for (int i = 0; i < 6 && n < (int)sizeof line - 3; i++) line[n++] = "links"[i];
    line[n] = 0;
    bar(h - 2, line, LGREY, BLACK);

    /* the bottom line: what you can type, or what you are typing */
    if (inputting) {
        n = 0;
        line[n++] = '>'; line[n++] = ' ';
        for (int i = 0; input[i] && n < wide - 2; i++) line[n++] = input[i];
        line[n++] = '_';
        line[n] = 0;
        bar(h - 1, line, BLACK, LGREY);
    } else if (msg[0]) {
        bar(h - 1, msg, YELLOW, BLACK);
    } else {
        bar(h - 1, showing_links
              ? "number+enter open   l page   q quit"
              : "number+enter link   g address   l links   b back   r reload   q quit",
            BLACK, LGREY);
    }

    /* and the page itself */
    mem_set(&W, 0, sizeof W);
    W.w = wide;
    W.first = top;
    W.count = rows;
    W.mode = showing_links ? W_LINKS : W_PAGE;
    W.attr = A_PLAIN;
    walk_page(&W);

    if (nlines == 0) {
        api->setcolor(DGREY, BLACK);
        api->gotoxy(2, 2);
        api->puts(msg[0] ? msg : "this page is empty");
        api->setcolor(LGREY, BLACK);
    }
}

/* ---------------------------------------------------------------- */
/*  loading                                                          */
/* ---------------------------------------------------------------- */

static const char *home_html(void);     /* the start page, at the bottom */

static void push_hist(const char *u)
{
    if (hpos < nhist - 1) nhist = hpos + 1;     /* a branch: drop the rest */
    if (nhist >= HIST) {
        for (int i = 0; i < HIST - 1; i++) copy_str(hist[i], hist[i + 1], UMAX);
        nhist = HIST - 1;
    }
    copy_str(hist[nhist], u, UMAX);
    hpos = nhist;
    nhist++;
}

static void load(const char *u)
{
    static char want[UMAX];
    static char tmp[UMAX];
    u32 got = 0;

    copy_str(want, u, sizeof want);
    if (str_ncmp(want, "http://", 7) && str_ncmp(want, "https://", 8) &&
        str_ncmp(want, "about:", 6)) {
        copy_str(tmp, "http://", sizeof tmp);
        cat_str(tmp, want, sizeof tmp);
        copy_str(want, tmp, sizeof want);
    }
    strip_frag(want);

    copy_str(url, want, sizeof url);
    title[0] = 0;
    status[0] = 0;
    msg[0] = 0;
    plen = 0;
    page[0] = 0;
    top = 0;
    nlinks = 0;
    showing_links = 0;

    if (nhist == 0 || str_cmp(hist[hpos], want)) push_hist(want);

    /* "about:start" and friends never leave the machine */
    if (!str_ncmp(want, "about:", 6)) {
        copy_str(page, home_html(), (int)pagesize);
        plen = (u32)app_strlen(page);
        if (plen >= pagesize) plen = pagesize - 1;
        page[plen] = 0;
        return;
    }

    int r = api->net_get(want, page, pagesize, &got, status, sizeof status);
    if (r == 0) {
        plen = got;
        if (plen >= pagesize) plen = pagesize - 1;
        page[plen] = 0;
    } else {
        plen = 0;
        page[0] = 0;
        switch (r) {
        case -1: copy_str(msg, "no network card, or no address yet - type 'dhcp' in the shell",
                          sizeof msg); break;
        case -2: copy_str(msg, "that name does not resolve - no dns answer",
                          sizeof msg); break;
        case -3: copy_str(msg, "nothing answered on the other end", sizeof msg); break;
        case -10: {
            const char *why = api->tls_error();
            copy_str(msg, why && why[0] ? why
                                        : "the encrypted connection failed",
                     sizeof msg);
            break;
        }
        default: copy_str(msg, "the page did not arrive", sizeof msg); break;
        }
    }
}

/* Find door number n and walk through it. */
static void follow(int n)
{
    static char href[UMAX];
    static char full[UMAX];

    mem_set(&W, 0, sizeof W);
    W.w = 80;
    W.mode = W_PICK;
    W.want = n;
    W.out = href;
    W.outmax = sizeof href;
    walk_page(&W);

    if (!W.got_it) {
        copy_str(msg, "there is no link with that number", sizeof msg);
        return;
    }
    resolve(url, href, full, sizeof full);
    load(full);
}

/* ---------------------------------------------------------------- */
/*  the page you get when there is nothing else to show              */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */

static void download_page(void)
{
    char name[13];
    int i = 0, last = -1, nd = 0;

    while (url[i] && i < (int)sizeof url) {
        if (url[i] == '/') last = i;
        i++;
    }
    i = last + 1;
    if (i < (int)sizeof url)
        while (url[i] && url[i] != '?' && url[i] != '#' && nd < 12) {
            char c = url[i++];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == '_')
                name[nd++] = c;
        }
    if (nd < 4 || name[nd - 4] != '.') {
        name[nd++] = '.'; name[nd++] = 'T'; name[nd++] = 'X'; name[nd++] = 'T';
    }
    name[nd] = 0;
    if (!plen) {
        copy_str(msg, "nothing here to keep", sizeof msg);
        return;
    }
    if (api->fs_write(name, page, (u32)plen) >= 0) {
        copy_str(msg, "saved as ", sizeof msg);
        cat_str(msg, name, sizeof msg);
        cat_str(msg, " -- it is in the ramdisk now", sizeof msg);
    } else {
        copy_str(msg, "the disk would not take it", sizeof msg);
    }
}

int app_main(int argc, char **argv)
{
    static const u32 sizes[4] = { 32768u, 16384u, 8192u, 4096u };

    for (int i = 0; i < 4 && !page; i++) {
        page = (char *)api->malloc(sizes[i]);
        if (page) pagesize = sizes[i];
    }
    if (!page) {
        api->puts("not enough memory for a browser\n");
        return 1;
    }
    page[0] = 0;

    wide = api->width();
    api->cursor(0);

    load(argc > 1 ? argv[1] : "about:start");

    for (;;) {
        draw();

        int k = api->getkey();

        if (inputting) {
            if (k == '\n' || k == '\r') {
                inputting = 0;
                if (inlen > 0) {
                    int digits = 1;
                    for (int i = 0; i < inlen; i++)
                        if (input[i] < '0' || input[i] > '9') digits = 0;
                    if (digits) follow(api_atoi(input));
                    else load(input);
                }
                inlen = 0; input[0] = 0;
            } else if (k == '\b') {
                if (inlen > 0) { inlen--; input[inlen] = 0; }
            } else if (k == 27) {
                inputting = 0; inlen = 0; input[0] = 0;
            } else if (k >= 32 && k < 127 && inlen < (int)sizeof input - 1) {
                input[inlen++] = (char)k;
                input[inlen] = 0;
            }
            continue;
        }

        switch (k) {
        case 'q': case 'Q': case 27: case KEY_F10:
            api->cursor(1);
            api->clear();
            return 0;
        case KEY_DOWN: case 'j':
            if (top + rows < nlines) top++;
            break;
        case KEY_UP: case 'k':
            if (top > 0) top--;
            break;
        case KEY_PGDN:
            top += rows - 1;
            if (top > nlines - rows) top = nlines - rows;
            if (top < 0) top = 0;
            break;
        case KEY_PGUP:
            top -= rows - 1;
            if (top < 0) top = 0;
            break;
        case KEY_HOME: top = 0; break;
        case KEY_END:
            top = nlines - rows; if (top < 0) top = 0; break;
        case 'b': case KEY_LEFT:
            if (hpos > 0) { hpos--; load(hist[hpos]); }
            break;
        case 'f': case KEY_RIGHT:
            if (hpos < nhist - 1) { hpos++; load(hist[hpos]); }
            break;
        case 'r': case 'R':
            load(url);
            break;
        case 'd': case 'D':
            download_page();
            break;
        case 'l': case 'L':
            showing_links = !showing_links;
            top = 0;
            break;
        case 'g': case 'G': case 'o':
            inputting = 1; inlen = 0; input[0] = 0;
            break;
        case 'h': case '?':
            load("about:start");
            break;
        default:
            if (k >= '0' && k <= '9') {
                inputting = 1;
                input[0] = (char)k;
                input[1] = 0;
                inlen = 1;
            }
            break;
        }
    }
}

/* The start page is kept at the bottom of the file so that the browser
 * itself reads like a browser and not like a wall of text. It is ordinary
 * html, which means it goes through exactly the same parser as a page that
 * came off the wire -- if this renders, the parser works. */
static const char *home_html(void)
{
    static const char *h =
    "<html><head><title>PicoOS browser</title></head><body>\n"
    "<h1>PicoOS browser</h1>\n"
    "<p>This is a web browser that runs inside a 32-bit kernel you can boot\n"
    "from a USB stick. It has no pictures, no javascript and no mouse: it\n"
    "reads the page, keeps the headings and the paragraphs and the lists,\n"
    "numbers every link it finds, and lets you type a number to follow one.\n"
    "</p>\n"
    "<h2>How to drive it</h2>\n"
    "<ul>\n"
    "<li>type a number, then enter, to open that link</li>\n"
    "<li>press g, type an address, press enter</li>\n"
    "<li>press l to see every link on the page with its number</li>\n"
    "<li>cursor keys, page up and page down scroll, b goes back</li>\n"
    "<li>press q to leave</li>\n"
    "</ul>\n"
    "<h2>About the encrypted web</h2>\n"
    "<p>https works. The kernel does the TLS 1.2 handshake itself: it reads\n"
    "the certificate, seals a secret with the server's key, agrees on a\n"
    "cipher and encrypts everything after. What it does not do is check that\n"
    "the certificate belongs to the name you typed -- that wants a list of\n"
    "authorities this machine has no room for -- and it cannot talk to a\n"
    "server that insists on ciphers newer than 2011. Most sites are fine.\n"
    "</p>\n"
    "<h2>Somewhere to start</h2>\n"
    "<ul>\n"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch - the first web page</a></li>\n"
    "<li><a href=\"http://neverssl.com/\">neverssl.com - a site that stays on http</a></li>\n"
    "<li><a href=\"http://example.com/\">example.com</a></li>\n"
    "</ul>\n"
    "<hr>\n"
    "<p>Everything you see here was drawn by one function that walks the\n"
    "page from the beginning and lays the words out in a column. That is\n"
    "the whole renderer.</p>\n"
    "</body></html>\n";
    return h;
}
