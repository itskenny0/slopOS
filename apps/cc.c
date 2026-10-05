/* cc.pico -- a C compiler, on the machine itself.
 *
 * Not a dialect with C clothes: real C, the small solid corner of it a
 * 1987 programmer would recognise. int and char, pointers and arrays,
 * functions with arguments and return values, if/else, while, do, for,
 * break and continue, ++ and --, && and ||, the whole precedence
 * ladder, and calls into the operating system -- putc, puts, beep, the
 * pixel calls, the clock -- compiled to 32-bit x86 machine code and
 * run, right here.
 *
 * Two passes, the usual two. The first walks the text and hangs every
 * statement on a tree; the second walks the tree and emits bytes.
 * Calls to functions not compiled yet are left half-written and patched
 * when the last function is in, which is what makes mutual recursion
 * work without planning ahead.
 *
 * Every buffer lives on the heap and goes back the moment compiling
 * stops: a compiler is a guest, not a resident. And the machine can
 * compile a program that beeps and counts, which is the whole point --
 * edit, F5, watch it run.
 */


#include "picoapp.h"

#define KEY_ESC 27

/* ---- sizes: chosen so a 300 KB machine can breathe ------------------- */
#define SRCMAX   12288
#define NDMAX    900
#define CODEMAX  24576
#define GDATAMAX 2048
#define POOLMAX  4096
#define SYMAX    160
#define FUNMAX   24
#define LABMAX   250
#define FIXMAX   512
#define PTMAX    240

/* ---- the tree --------------------------------------------------------- */

enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_BCALL, N_BIN, N_ASSIGN,
    N_NOT, N_NEG, N_ADDR, N_DEREF, N_IDX,
    N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC,
    N_SEQ, N_BLOCK, N_IF, N_WHILE, N_DO, N_FOR, N_RET,
    N_BREAK, N_CONT, N_EXPR, N_DECL
};

enum {
    B_OROR, B_ANDAND, B_OR, B_XOR, B_AND,
    B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE,
    B_SHL, B_SHR, B_ADD, B_SUB, B_MUL, B_DIV, B_MOD
};

typedef struct {
    u8   op;
    u8   byte;            /* a char-sized value: load/store one byte   */
    u8   scale;           /* doubles as a label for the loop nodes     */
    s8   kind;            /* doubles as the out-label for loops        */
    u16  a, b;            /* children                                  */
    int  i, j;            /* value / symbol / child / label            */
} node_t;

typedef struct {
    int  name;            /* offset of the name in the pool            */
    int  off;             /* global: gdata offset, local: -(ebp offset) */
    u8   is_ptr, is_char, is_array;
    u8   scope;           /* 0 global, 1 local                         */
    u8   strinit;         /* 1 = initialised from a string literal     */
    int  init;            /* numeric initialiser, or 0                 */
    u8   has_init;
} sym_t;

typedef struct {
    int  name;
    int  params;
    int  code_off;
    int  body;
    int  nloc;
    int  end_lab;
    u8   defined;
} fn_t;

/* ---- state, all reset before every compile ---------------------------- */

static node_t *nd;
static int     nn;
static sym_t  *sy;
static int     nsy, sy_base;
static fn_t   *fns;
static int     nfn, fn_main;
static u8     *C;
static int     cp;
static int     gdata_n, pool_n;
static int     labs[LABMAX];
static int     nlab;
static struct { int pos, lab; } fix[FIXMAX];
static int     nfix;
static struct { int pos, fn; }  pend[PTMAX];
static int     npend;
static int     brk_l[16], cont_l[16], dpth;
static int     loc_bytes;
static int     f_end_lab;          /* where return jumps to             */
static char   *poolp;              /* the string pool, while lexing     */

static char   *src;
static int     sp, line, err_line;

/* ---- lexer ------------------------------------------------------------ */

enum {
    T_EOF = 256, T_NAME, T_NUM, T_STR,
    T_INC, T_DEC, T_SHL, T_SHR, T_LE, T_GE, T_EQ, T_NE,
    T_ANDAND, T_OROR, T_PLUSEQ, T_MINUSEQ, T_MULEQ, T_DIVEQ, T_MODEQ,
    T_KINT, T_KCHAR, T_KVOID, T_KIF, T_KELSE, T_KWHILE, T_KDO, T_KFOR,
    T_KRETURN, TKBREAK, TKCONT
};

static int   tk;
static int   tki, tkstr;
static char  tkname[32];

static const char *kwords[] = {
    "int", "char", "void", "if", "else", "while", "do", "for",
    "return", "break", "continue", 0
};
static const int ktok[] = {
    T_KINT, T_KCHAR, T_KVOID, T_KIF, T_KELSE, T_KWHILE, T_KDO, T_KFOR,
    T_KRETURN, TKBREAK, TKCONT
};

static int streq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static int pool_put(const char *s)
{
    int off = pool_n;
    while (*s && pool_n < POOLMAX - 1) poolp[pool_n++] = *s++;
    poolp[pool_n++] = 0;
    return off;
}

static int lex(void)
{
    for (;;) {
        char c = src[sp];
        if (!c) { tk = T_EOF; return tk; }
        if (c == '\n') { line++; sp++; continue; }   /* newlines flow    */
        if (c == ' ' || c == '\t' || c == '\r') { sp++; continue; }
        if (c == '/' && src[sp + 1] == '/') {
            while (src[sp] && src[sp] != '\n') sp++;
            continue;
        }
        if (c == '/' && src[sp + 1] == '*') {
            sp += 2;
            while (src[sp] && !(src[sp] == '*' && src[sp + 1] == '/')) {
                if (src[sp] == '\n') line++;
                sp++;
            }
            if (src[sp]) sp += 2;
            continue;
        }
        break;
    }

    err_line = line;
    char c = src[sp];

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int i = 0;
        while ((src[sp] >= 'a' && src[sp] <= 'z') ||
               (src[sp] >= 'A' && src[sp] <= 'Z') ||
               (src[sp] >= '0' && src[sp] <= '9') || src[sp] == '_') {
            if (i < (int)sizeof tkname - 1) tkname[i++] = src[sp];
            sp++;
        }
        tkname[i] = 0;
        for (int k = 0; kwords[k]; k++)
            if (streq(tkname, kwords[k])) { tk = ktok[k]; return tk; }
        tk = T_NAME; return tk;
    }

    if (c >= '0' && c <= '9') {
        int v = 0, hex = 0;
        if (c == '0' && (src[sp + 1] == 'x' || src[sp + 1] == 'X')) {
            hex = 1; sp += 2;
        }
        for (;;) {
            char d = src[sp];
            int dv;
            if (d >= '0' && d <= '9') dv = d - '0';
            else if (hex && d >= 'a' && d <= 'f') dv = d - 'a' + 10;
            else if (hex && d >= 'A' && d <= 'F') dv = d - 'A' + 10;
            else break;
            v = v * (hex ? 16 : 10) + dv;
            sp++;
        }
        tki = v; tk = T_NUM; return tk;
    }

    if (c == '\'') {
        sp++;
        int v = src[sp++];
        if (v == '\\') {
            char e = src[sp++];
            v = e == 'n' ? 10 : e == 't' ? 9 : e == '0' ? 0 : e;
        }
        if (src[sp] == '\'') sp++;
        tki = v; tk = T_NUM; return tk;
    }

    if (c == '"') {
        sp++;
        int off = pool_n;
        while (src[sp] && src[sp] != '"') {
            char ch = src[sp++];
            if (ch == '\\') {
                char e = src[sp++];
                ch = e == 'n' ? 10 : e == 't' ? 9 : e == '0' ? 0 : e;
            }
            if (pool_n < POOLMAX - 1) poolp[pool_n++] = ch;
        }
        if (src[sp] == '"') sp++;
        poolp[pool_n++] = 0;
        tkstr = off; tk = T_STR; return tk;
    }

    sp++;
    switch (c) {
    case '+': if (src[sp] == '+') { sp++; tk = T_INC; return tk; }
              if (src[sp] == '=') { sp++; tk = T_PLUSEQ; return tk; }
              tk = '+'; return tk;
    case '-': if (src[sp] == '-') { sp++; tk = T_DEC; return tk; }
              if (src[sp] == '=') { sp++; tk = T_MINUSEQ; return tk; }
              tk = '-'; return tk;
    case '*': if (src[sp] == '=') { sp++; tk = T_MULEQ; return tk; }
              tk = '*'; return tk;
    case '/': if (src[sp] == '=') { sp++; tk = T_DIVEQ; return tk; }
              tk = '/'; return tk;
    case '%': if (src[sp] == '=') { sp++; tk = T_MODEQ; return tk; }
              tk = '%'; return tk;
    case '<': if (src[sp] == '=') { sp++; tk = T_LE; return tk; }
              if (src[sp] == '<') { sp++; tk = T_SHL; return tk; }
              tk = '<'; return tk;
    case '>': if (src[sp] == '=') { sp++; tk = T_GE; return tk; }
              if (src[sp] == '>') { sp++; tk = T_SHR; return tk; }
              tk = '>'; return tk;
    case '=': if (src[sp] == '=') { sp++; tk = T_EQ; return tk; }
              tk = '='; return tk;
    case '!': if (src[sp] == '=') { sp++; tk = T_NE; return tk; }
              tk = '!'; return tk;
    case '&': if (src[sp] == '&') { sp++; tk = T_ANDAND; return tk; }
              tk = '&'; return tk;
    case '|': if (src[sp] == '|') { sp++; tk = T_OROR; return tk; }
              tk = '|'; return tk;
    }
    tk = (u8)c; return tk;
}

/* ---- emission ---------------------------------------------------------- */

static void e1(int b) { C[cp++] = (u8)b; }
static void e32(u32 v)
{
    C[cp++] = (u8)v; C[cp++] = (u8)(v >> 8);
    C[cp++] = (u8)(v >> 16); C[cp++] = (u8)(v >> 24);
}

static int lab_new(void) { return nlab++; }

static void jcc_fwd(int cond, int l)
{
    e1(0x0F); e1(cond); e32(0);
    fix[nfix].pos = cp; fix[nfix].lab = l; nfix++;
}
static void jmp_fwd(int l)
{
    e1(0xE9); e32(0);
    fix[nfix].pos = cp; fix[nfix].lab = l; nfix++;
}
static void lab_set(int l) { labs[l] = cp; }

/* ---- errors ------------------------------------------------------------ */

static int cfail(const char *msg)
{
    char t[12], out[12];
    int i = 0, n = 0;
    u32 v = (u32)err_line;
    api->puts("cc: ");
    api->puts(msg);
    api->puts(" on line ");
    if (!v) t[i++] = '0';
    while (v && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) out[n++] = t[--i];
    out[n] = 0;
    api->puts(out);
    api->puts("\n");
    return 0;
}

/* ---- helpers ----------------------------------------------------------- */

static int find_sym(const char *n)
{
    for (int i = nsy - 1; i >= 0; i--)
        if (streq(poolp + sy[i].name, n)) return i;
    return -1;
}
static int find_fn(const char *n)
{
    for (int i = 0; i < nfn; i++)
        if (streq(poolp + fns[i].name, n)) return i;
    return -1;
}

static int node(int op)
{
    if (nn >= NDMAX) { cfail("program too large"); return 0; }
    nd[nn].op = (u8)op;
    nd[nn].byte = 0; nd[nn].scale = 4; nd[nn].kind = 0;
    nd[nn].a = nd[nn].b = 0; nd[nn].i = nd[nn].j = 0;
    return nn++;
}

/* ---- builtins ---------------------------------------------------------- */

static const char *bnames[] = {
    "putc", "puts", "getkey", "key", "ticks", "sleep", "rand", "beep",
    "clrscr", "gotoxy", "setcolor", "pixel", "fill", "mouse",
    "width", "height", "time", "sound", 0
};

static int builtin_id(const char *n)
{
    for (int i = 0; bnames[i]; i++)
        if (streq(bnames[i], n)) return i + 1;
    return -1;
}

/* the bridge: generated code calls glue with the id and the arguments
 * already pushed, and glue finds them through its own frame */
/* the frame must be real: glue reads its caller's stack through it */
static u32 __attribute__((optimize("no-omit-frame-pointer"))) glue(void)
{
    u32 *st = (u32 *)__builtin_frame_address(0);
    u32 id = st[2];              /* first thing pushed                  */
    u32 a0 = st[3], a1 = st[4], a2 = st[5], a3 = st[6], a4 = st[7];

    switch (id) {
    case 1:  api->putc((char)a0); return 0;
    case 2:  api->puts((const char *)a0); return 0;
    case 3:  return (u32)api->getkey();
    case 4:  return (u32)api->poll();
    case 5:  return api->ticks();
    case 6:  api->sleep(a0); return 0;
    case 7:  { u32 r = api->rand(); return a0 ? r % a0 : r; }
    case 8:  api->beep(a0, a1); return 0;
    case 9:  api->clear(); return 0;
    case 10: api->gotoxy((int)a0, (int)a1); return 0;
    case 11: api->setcolor((u8)a0, (u8)a1); return 0;
    case 12: api->pixel((int)a0, (int)a1, (int)a2); return 0;
    case 13: api->fill_rect((int)a0, (int)a1, (int)a2, (int)a3, (int)a4);
             return 0;
    case 14: {
        int x = 0, y = 0, b = 0;
        int r = api->mouse_px(&x, &y, &b);      /* all three, buttons too */
        if (a0) *(int *)a0 = x;
        if (a1) *(int *)a1 = y;
        if (a2) *(int *)a2 = b;
        return (u32)r;
    }
    case 15: return (u32)api->width();
    case 16: return (u32)api->height();
    case 17: {
        int h = 0, m = 0, s = 0;
        api->rtc(&h, &m, &s);
        if (a0) *(int *)a0 = h;
        if (a1) *(int *)a1 = m;
        if (a2) *(int *)a2 = s;
        return 0;
    }
    case 18: api->sound((int)(a0 == 0)); return 0;
    }
    return 0;
}

/* ---- parser ------------------------------------------------------------ */

static int expr(void);
static int statement(void);

static int fncall_args(int *nargs)
{
    int chain = 0, n = 0;
    if (tk != ')') {
        for (;;) {
            int e = expr();
            int c = node(N_SEQ);
            nd[c].a = (u16)e;
            nd[c].b = (u16)chain;
            chain = c;
            n++;
            if (tk != ',') break;
            lex();
        }
    }
    *nargs = n;
    return chain;
}

static int primary(void)
{
    if (tk == T_NUM) {
        int n = node(N_NUM);
        nd[n].i = tki;
        lex();
        return n;
    }
    if (tk == T_STR) {
        int n = node(N_STR);
        nd[n].i = tkstr;
        lex();
        return n;
    }
    if (tk == '(') {
        lex();
        int e = expr();
        if (tk != ')') return cfail("expected )");
        lex();
        return e;
    }
    if (tk == T_NAME) {
        char name[32];
        int i = 0;
        while (tkname[i] && i < (int)sizeof name - 1) { name[i] = tkname[i]; i++; }
        name[i] = 0;
        lex();
        if (tk == '(') {
            lex();
            int nargs = 0;
            int chain = fncall_args(&nargs);
            if (tk != ')') return cfail("expected )");
            lex();
            int fi = find_fn(name);
            int n = node(fi >= 0 ? N_CALL : N_BCALL);
            nd[n].a = (u16)chain;
            nd[n].j = nargs;
            nd[n].i = fi >= 0 ? fi : builtin_id(name);
            if (fi < 0 && nd[n].i < 0) return cfail("unknown function");
            return n;
        }
        int s = find_sym(name);
        if (s < 0) return cfail("unknown name");
        int n = node(N_VAR);
        nd[n].i = s;
        if (sy[s].is_char && !sy[s].is_ptr && !sy[s].is_array)
            nd[n].byte = 1;
        if (sy[s].is_array)
            nd[n].scale = (u8)(sy[s].is_char ? 1 : 4);
        return n;
    }
    return cfail("unexpected token");
}

static int postfix(void)
{
    int n = primary();
    for (;;) {
        if (tk == '[') {
            lex();
            int idx = expr();
            if (tk != ']') return cfail("expected ]");
            lex();
            int m = node(N_IDX);
            nd[m].a = (u16)n;
            nd[m].b = (u16)idx;
            /* char things are one byte apart, everything else four    */
            int ch = 0;
            if (nd[n].op == N_VAR &&
                (sy[nd[n].i].is_char && (sy[nd[n].i].is_array || sy[nd[n].i].is_ptr)))
                ch = 1;
            nd[m].scale = (u8)(ch ? 1 : 4);
            nd[m].byte = (u8)ch;
            n = m;
        } else if (tk == T_INC) {
            lex();
            int m = node(N_POSTINC); nd[m].a = (u16)n; n = m;
        } else if (tk == T_DEC) {
            lex();
            int m = node(N_POSTDEC); nd[m].a = (u16)n; n = m;
        } else break;
    }
    return n;
}

static int unary(void)
{
    if (tk == '-') { lex(); int m = node(N_NEG); nd[m].a = (u16)unary(); return m; }
    if (tk == '!') { lex(); int m = node(N_NOT); nd[m].a = (u16)unary(); return m; }
    if (tk == '*') {
        lex();
        int e = unary();
        int m = node(N_DEREF);
        nd[m].a = (u16)e;
        if (nd[e].op == N_VAR &&
            sy[nd[e].i].is_char && sy[nd[e].i].is_ptr)
            nd[m].byte = 1;
        return m;
    }
    if (tk == '&') { lex(); int m = node(N_ADDR); nd[m].a = (u16)unary(); return m; }
    if (tk == T_INC) { lex(); int m = node(N_PREINC); nd[m].a = (u16)unary(); return m; }
    if (tk == T_DEC) { lex(); int m = node(N_PREDEC); nd[m].a = (u16)unary(); return m; }
    return postfix();
}

static int binop_of(int t)
{
    switch (t) {
    case T_OROR: return B_OROR;
    case T_ANDAND: return B_ANDAND;
    case '|': return B_OR;
    case '^': return B_XOR;
    case '&': return B_AND;
    case T_EQ: return B_EQ;
    case T_NE: return B_NE;
    case '<': return B_LT;
    case T_LE: return B_LE;
    case '>': return B_GT;
    case T_GE: return B_GE;
    case T_SHL: return B_SHL;
    case T_SHR: return B_SHR;
    case '+': return B_ADD;
    case '-': return B_SUB;
    case '*': return B_MUL;
    case '/': return B_DIV;
    case '%': return B_MOD;
    }
    return -1;
}

static const int lvl_ops[10][5] = {
    { T_OROR, 0 },
    { T_ANDAND, 0 },
    { '|', 0 },
    { '^', 0 },
    { '&', 0 },
    { T_EQ, T_NE, 0 },
    { '<', T_LE, '>', T_GE, 0 },
    { T_SHL, T_SHR, 0 },
    { '+', '-', 0 },
    { '*', '/', '%', 0 },
};

static int binlvl(int lvl)
{
    int n = (lvl >= 10) ? unary() : binlvl(lvl + 1);
    for (;;) {
        int hit = 0;
        if (lvl < 10)
            for (int i = 0; i < 5 && lvl_ops[lvl][i]; i++)
                if (tk == lvl_ops[lvl][i]) { hit = 1; break; }
        if (!hit) break;
        int op = binop_of(tk);
        lex();
        int r = (lvl >= 10) ? unary() : binlvl(lvl + 1);
        int m = node(N_BIN);
        nd[m].a = (u16)n; nd[m].b = (u16)r; nd[m].i = op;
        n = m;
    }
    return n;
}

static int expr(void)
{
    int n = binlvl(0);
    if (tk == '=') {
        lex();
        int m = node(N_ASSIGN);
        nd[m].a = (u16)n; nd[m].b = (u16)expr();
        return m;
    }
    if (tk == T_PLUSEQ || tk == T_MINUSEQ || tk == T_MULEQ ||
        tk == T_DIVEQ || tk == T_MODEQ) {
        int which = tk;
        lex();
        int r = expr();
        int b = node(N_BIN);
        nd[b].a = (u16)n; nd[b].b = (u16)r;
        nd[b].i = which == T_PLUSEQ ? B_ADD : which == T_MINUSEQ ? B_SUB :
                  which == T_MULEQ ? B_MUL : which == T_DIVEQ ? B_DIV : B_MOD;
        int m = node(N_ASSIGN);
        nd[m].a = (u16)n; nd[m].b = (u16)b;
        return m;
    }
    return n;
}

/* ---- declarations ------------------------------------------------------- */

/* parse one declarator; infunc: 1 inside a function (locals), else globals.
 * returns a N_DECL node (j = init expression) or 0 on error */
static int declarator(int is_char, int infunc, int *took_bytes)
{
    int stars = 0;
    while (tk == '*') { stars++; lex(); }
    if (tk != T_NAME) return cfail("expected a name");

    char name[32];
    int i = 0;
    while (tkname[i] && i < (int)sizeof name - 1) { name[i] = tkname[i]; i++; }
    name[i] = 0;
    lex();

    int is_array = 0, count = 1;
    if (tk == '[') {
        lex();
        is_array = 1;
        if (tk == T_NUM) { count = tki; lex(); }
        if (tk == ']') lex();
        if (count < 1) count = 1;
    }

    int init_expr = 0;
    int strinit = -1;
    if (tk == '=') {
        lex();
        if (tk == T_STR) {
            strinit = tkstr;
            lex();
        } else {
            init_expr = expr();
        }
    }

    if (nsy >= SYMAX) return cfail("too many names");
    int q = nsy++;
    sy[q].name = pool_put(name);
    sy[q].is_ptr  = (u8)(stars > 0);
    sy[q].is_char = (u8)is_char;
    sy[q].is_array = (u8)is_array;
    sy[q].strinit = (u8)(strinit >= 0);
    sy[q].init = strinit;
    sy[q].has_init = (u8)(strinit >= 0 || init_expr != 0);

    int sz = is_array ? (is_char ? count : count * 4) : 4;

    if (infunc) {
        sy[q].scope = 1;
        loc_bytes += sz;
        sy[q].off = -loc_bytes;
        if (loc_bytes > 120) return cfail("too many locals");
        if (strinit >= 0) {
            int s = node(N_STR);
            nd[s].i = strinit;
            init_expr = s;
        }
    } else {
        sy[q].scope = 0;
        sy[q].off = gdata_n;
        gdata_n += sz;
        if (gdata_n > GDATAMAX) return cfail("out of global room");
        /* numeric inits of plain globals are data, not code */
        if (init_expr && init_expr < NDMAX &&
            nd[init_expr].op == N_NUM && !is_array) {
            *(u32 *)(C + CODEMAX + sy[q].off) = (u32)nd[init_expr].i;
            init_expr = 0;
        }
    }

    int d = node(N_DECL);
    nd[d].i = q;
    nd[d].j = infunc ? init_expr : 0;
    if (took_bytes) *took_bytes = sz;
    return d;
}

static int is_type_tok(int t)
{
    return t == T_KINT || t == T_KCHAR || t == T_KVOID;
}

/* ---- statements --------------------------------------------------------- */

static int block(void)
{
    if (tk != '{') return cfail("expected {");
    lex();
    int first = 0, last = 0;
    while (tk != '}' && tk != T_EOF) {
        int s = statement();
        if (!s) return 0;
        if (s == -1) continue;                 /* pure semicolon       */
        /* statements ride in wrappers so their own b stays free        */
        int w = node(N_SEQ);
        nd[w].a = (u16)s;
        if (last) nd[last].b = (u16)w; else first = w;
        last = w;
    }
    if (tk != '}') return cfail("expected }");
    lex();
    int n = node(N_BLOCK);
    nd[n].a = (u16)first;
    return n;
}

static int statement(void)
{
    if (tk == '{') return block();

    if (is_type_tok(tk)) {
        int is_char = (tk == T_KCHAR);
        lex();
        if (tk == ';') { lex(); return -1; }    /* bare type: nothing  */
        int first = 0, last = 0;
        for (;;) {
            int d = declarator(is_char, 1, 0);
            if (!d) return 0;
            if (last) nd[last].b = (u16)d; else first = d;
            last = d;
            if (tk != ',') break;
            lex();
        }
        if (tk == ';') lex();
        return first;
    }

    if (tk == T_KIF) {
        lex();
        if (tk != '(') return cfail("expected (");
        lex();
        int c = expr();
        if (tk != ')') return cfail("expected )");
        lex();
        int s = statement();
        if (!s) return 0;
        if (s == -1) s = 0;
        int n = node(N_IF);
        nd[n].a = (u16)c; nd[n].b = (u16)s;
        if (tk == T_KELSE) {
            lex();
            int e = statement();
            if (e == -1) e = 0;
            nd[n].i = e;
        }
        return n;
    }

    if (tk == T_KWHILE) {
        lex();
        if (tk != '(') return cfail("expected (");
        lex();
        int c = expr();
        if (tk != ')') return cfail("expected )");
        lex();
        int top = lab_new(), out = lab_new();
        if (dpth >= 16) return cfail("loops nested too deep");
        brk_l[dpth] = out; cont_l[dpth] = top; dpth++;
        int b = statement();
        dpth--;
        if (!b) return 0;
        if (b == -1) b = 0;
        int n = node(N_WHILE);
        nd[n].a = (u16)c; nd[n].b = (u16)b;
        nd[n].scale = (u8)top; nd[n].kind = (s8)out;
        return n;
    }

    if (tk == T_KDO) {
        lex();
        int top = lab_new(), out = lab_new();
        if (dpth >= 16) return cfail("loops nested too deep");
        brk_l[dpth] = out; cont_l[dpth] = top; dpth++;
        int b = statement();
        dpth--;
        if (!b) return 0;
        if (b == -1) b = 0;
        if (tk != T_KWHILE) return cfail("expected while");
        lex();
        if (tk != '(') return cfail("expected (");
        lex();
        int c = expr();
        if (tk != ')') return cfail("expected )");
        lex();
        if (tk == ';') lex();
        int n = node(N_DO);
        nd[n].a = (u16)b; nd[n].b = (u16)c;
        nd[n].scale = (u8)top; nd[n].kind = (s8)out;
        return n;
    }

    if (tk == T_KFOR) {
        lex();
        if (tk != '(') return cfail("expected (");
        lex();
        int ini = 0, cond = 0, step = 0;
        if (tk != ';') {
            /* for (int i = 0; ... is allowed: one declaration         */
            if (is_type_tok(tk)) {
                int is_char = (tk == T_KCHAR);
                lex();
                ini = declarator(is_char, 1, 0);
                if (!ini) return 0;
            } else {
                ini = node(N_EXPR);
                nd[ini].a = (u16)expr();
            }
        }
        if (tk != ';') return cfail("expected ;");
        lex();
        if (tk != ';') cond = expr();
        if (tk != ';') return cfail("expected ;");
        lex();
        if (tk != ')') step = node(N_EXPR), nd[step].a = (u16)expr();
        if (tk != ')') return cfail("expected )");
        lex();
        int top = lab_new(), stp = lab_new(), out = lab_new();
        if (dpth >= 16) return cfail("loops nested too deep");
        brk_l[dpth] = out; cont_l[dpth] = stp; dpth++;
        int b = statement();
        dpth--;
        if (!b) return 0;
        if (b == -1) b = 0;
        int n = node(N_FOR);
        nd[n].a = (u16)ini; nd[n].b = (u16)cond;
        nd[n].i = step; nd[n].j = b;
        nd[n].scale = (u8)top; nd[n].kind = (s8)out;
        /* stp rides in the init node's spare field                    */
        nd[ini ? ini : n].byte = (u8)stp;
        nd[n].byte = (u8)stp;
        return n;
    }

    if (tk == T_KRETURN) {
        lex();
        int e = 0;
        if (tk != ';') e = expr();
        if (tk != ';') return cfail("expected ;");
        lex();
        int n = node(N_RET);
        nd[n].a = (u16)e;
        nd[n].i = f_end_lab;
        return n;
    }

    if (tk == TKBREAK) {
        lex();
        if (tk == ';') lex();
        int n = node(N_BREAK);
        nd[n].i = dpth ? brk_l[dpth - 1] : 0;
        return n;
    }
    if (tk == TKCONT) {
        lex();
        if (tk == ';') lex();
        int n = node(N_CONT);
        nd[n].i = dpth ? cont_l[dpth - 1] : 0;
        return n;
    }

    if (tk == ';') { lex(); return -1; }

    int e = expr();
    if (tk != ';') return cfail("expected ;");
    lex();
    int n = node(N_EXPR);
    nd[n].a = (u16)e;
    return n;
}

/* ---- codegen ------------------------------------------------------------- */

static void gen_addr(int n);
static void gen(int n);
static void gen_stmt(int n);

static void gen_addr(int n)
{
    switch (nd[n].op) {
    case N_VAR: {
        sym_t *s = &sy[nd[n].i];
        if (s->scope == 1) {
            e1(0x8D); e1(0x45); e1((u8)s->off);       /* lea eax,[ebp+d8] */
        } else {
            e1(0xB8); e32((u32)(size_t)(C + CODEMAX + s->off));
        }
        return;
    }
    case N_IDX: {
        int b = nd[n].a;
        if (nd[b].op == N_VAR && sy[nd[b].i].is_array)
            gen_addr(b);                              /* an array name    */
        else
            gen(b);                                   /* a pointer value  */
        e1(0x50);                                     /* push base        */
        gen(nd[n].b);
        if (nd[n].scale > 1) {
            e1(0xB9); e32((u32)nd[n].scale);          /* mov ecx,scale    */
            e1(0x0F); e1(0xAF); e1(0xC1);             /* imul eax,ecx     */
        }
        e1(0x5B);                                     /* pop ebx          */
        e1(0x01); e1(0xD8);                           /* add eax,ebx      */
        return;
    }
    case N_DEREF:
        gen(nd[n].a);
        return;
    }
    cfail("cannot assign to that");
}

static void load_val(u8 byte)
{
    if (byte) { e1(0x0F); e1(0xB6); e1(0x00); }       /* movzx eax,b[eax] */
    else      { e1(0x8B); e1(0x00); }                 /* mov eax,[eax]    */
}

static void store_val(u8 byte)
{
    if (byte) { e1(0x88); e1(0x18); }                 /* mov [eax],bl     */
    else      { e1(0x89); e1(0x18); }                 /* mov [eax],ebx    */
}

static void gen(int n)
{
    switch (nd[n].op) {
    case N_NUM:
        e1(0xB8); e32((u32)nd[n].i);
        return;
    case N_STR:
        e1(0xB8); e32((u32)(size_t)(poolp + nd[n].i));
        return;
    case N_VAR:
        if (sy[nd[n].i].is_array) { gen_addr(n); return; }
        gen_addr(n);
        load_val(nd[n].byte);
        return;
    case N_IDX:
    case N_DEREF:
        gen_addr(n);
        load_val(nd[n].byte);
        return;
    case N_ADDR:
        gen_addr(nd[n].a);
        return;
    case N_NOT:
        gen(nd[n].a);
        e1(0x85); e1(0xC0);                           /* test eax,eax     */
        e1(0x0F); e1(0x94); e1(0xC0);                 /* setz al          */
        e1(0x0F); e1(0xB6); e1(0xC0);                 /* movzx eax,al     */
        return;
    case N_NEG:
        gen(nd[n].a);
        e1(0xF7); e1(0xD8);                           /* neg eax          */
        return;
    case N_BIN: {
        int op = nd[n].i;
        if (op == B_ANDAND || op == B_OROR) {
            /* both operands are tested and branch to one meeting point:
             * short-circuit, and a clean 0 or 1 either way              */
            int other = lab_new(), end = lab_new();
            int jcd = (op == B_ANDAND) ? 0x84 : 0x85;  /* jz : jnz        */
            gen(nd[n].a);
            e1(0x85); e1(0xC0);
            jcc_fwd(jcd, other);
            gen(nd[n].b);
            e1(0x85); e1(0xC0);
            jcc_fwd(jcd, other);
            e1(0xB8); e32(op == B_ANDAND ? 1u : 0u);
            jmp_fwd(end);
            lab_set(other);
            e1(0xB8); e32(op == B_ANDAND ? 0u : 1u);
            lab_set(end);
            return;
        }
        gen(nd[n].a);
        e1(0x50);                                     /* push eax         */
        gen(nd[n].b);
        e1(0x89); e1(0xC3);                           /* mov ebx,eax      */
        e1(0x58);                                     /* pop eax          */
        switch (op) {
        case B_ADD: e1(0x01); e1(0xD8); break;
        case B_SUB: e1(0x29); e1(0xD8); break;
        case B_MUL: e1(0x0F); e1(0xAF); e1(0xC3); break;
        case B_DIV:
            /* cdq wipes edx: the divisor moves to ecx first            */
            e1(0x89); e1(0xD9);                       /* mov ecx,ebx      */
            e1(0x99);                                 /* cdq              */
            e1(0xF7); e1(0xF9);                       /* idiv ecx         */
            break;
        case B_MOD:
            e1(0x89); e1(0xD9); e1(0x99); e1(0xF7); e1(0xF9);
            e1(0x89); e1(0xD0);                       /* mov eax,edx      */
            break;
        case B_AND: e1(0x21); e1(0xD8); break;
        case B_OR:  e1(0x09); e1(0xD8); break;
        case B_XOR: e1(0x31); e1(0xD8); break;
        case B_SHL: e1(0x89); e1(0xD9); e1(0xD3); e1(0xE0); break;
        case B_SHR: e1(0x89); e1(0xD9); e1(0xD3); e1(0xF8); break;
        default: {
            /* the six comparisons, in B_ order                        */
            e1(0x39); e1(0xD8);                       /* cmp eax,ebx     */
            if (op == B_EQ) { e1(0x0F); e1(0x94); }
            else if (op == B_NE) { e1(0x0F); e1(0x95); }
            else if (op == B_LT) { e1(0x0F); e1(0x9C); }
            else if (op == B_LE) { e1(0x0F); e1(0x9E); }
            else if (op == B_GT) { e1(0x0F); e1(0x9F); }
            else { e1(0x0F); e1(0x9D); }
            e1(0xC0);
            e1(0x0F); e1(0xB6); e1(0xC0);             /* movzx eax,al    */
            break;
        }
        }
        return;
    }
    case N_ASSIGN:
        gen(nd[n].b);                                  /* the value       */
        e1(0x50);
        gen_addr(nd[n].a);
        e1(0x5B);                                      /* pop ebx         */
        store_val(nd[nd[n].a].byte);
        e1(0x89); e1(0xD8);                            /* eax = value     */
        return;
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: {
        int dec  = (nd[n].op == N_PREDEC || nd[n].op == N_POSTDEC);
        int post = (nd[n].op == N_POSTINC || nd[n].op == N_POSTDEC);
        gen_addr(nd[n].a);
        if (nd[nd[n].a].byte) { e1(0x0F); e1(0xB6); e1(0x18); }
        else { e1(0x8B); e1(0x18); }                   /* mov ebx,[eax]   */
        if (post) { e1(0x89); e1(0xD9); }              /* mov ecx,ebx     */
        e1(0x83); e1(dec ? 0xEB : 0xC3); e1(1);        /* sub/add ebx,1   */
        store_val(nd[nd[n].a].byte);
        if (post) { e1(0x89); e1(0xC8); }              /* mov eax,ecx     */
        else { e1(0x89); e1(0xD8); }                   /* mov eax,ebx     */
        return;
    }
    case N_CALL:
    case N_BCALL: {
        int nargs = nd[n].j;
        int k = nd[n].a;
        while (k) {                                    /* push the args   */
            gen(nd[k].a);
            e1(0x50);
            k = nd[k].b;
        }
        if (nd[n].op == N_BCALL) {
            e1(0xB8); e32((u32)nd[n].i);               /* mov eax,id      */
            e1(0x50);                                  /* push eax        */
            e1(0xBA); e32((u32)(size_t)glue);          /* mov edx,glue    */
            e1(0xFF); e1(0xD2);                        /* call edx        */
            nargs++;
        } else {
            int fi = nd[n].i;
            e1(0xE8); e32(0);                          /* call rel32      */
            if (fns[fi].defined) {
                u32 rel = (u32)(fns[fi].code_off - cp);
                C[cp-4] = (u8)rel; C[cp-3] = (u8)(rel >> 8);
                C[cp-2] = (u8)(rel >> 16); C[cp-1] = (u8)(rel >> 24);
            } else {
                pend[npend].pos = cp; pend[npend].fn = fi; npend++;
            }
        }
        e1(0x81); e1(0xC4); e32((u32)(4 * nargs));     /* add esp,4n      */
        return;
    }
    }
    cfail("internal: bad node");
}

static void gen_one(int n);

static void gen_stmt(int chain)
{
    for (int w = chain; w; w = nd[w].b)
        gen_one(nd[w].a);
}

static void gen_one(int n)
{
    switch (nd[n].op) {
    case N_BLOCK:
        gen_stmt(nd[n].a);
        return;
    case N_DECL:
        if (nd[n].j) {
            gen(nd[n].j);
            e1(0x89); e1(0x45); e1((u8)sy[nd[n].i].off);  /* [ebp+off]  */
        }
        return;
    case N_EXPR:
        gen(nd[n].a);
        return;
    case N_IF: {
        int le = lab_new(), lend = lab_new();
        gen(nd[n].a);
        e1(0x85); e1(0xC0);
        jcc_fwd(0x84, le);                             /* jz else         */
        gen_one(nd[n].b);
        jmp_fwd(lend);
        lab_set(le);
        if (nd[n].i) gen_one(nd[n].i);
        lab_set(lend);
        return;
    }
    case N_WHILE: {
        int top = nd[n].scale, out = (u8)nd[n].kind;
        lab_set(top);
        if (nd[n].a) gen(nd[n].a);
        e1(0x85); e1(0xC0);
        jcc_fwd(0x84, out);                            /* jz out          */
        gen_one(nd[n].b);
        jmp_fwd(top);
        lab_set(out);
        return;
    }
    case N_DO: {
        int top = nd[n].scale;
        lab_set(top);
        gen_one(nd[n].a);
        gen(nd[n].b);
        e1(0x85); e1(0xC0);
        jcc_fwd(0x85, top);                            /* jnz top         */
        lab_set((u8)nd[n].kind);
        return;
    }
    case N_FOR: {
        int top = nd[n].scale, out = (u8)nd[n].kind, stp = nd[n].byte;
        if (nd[n].a) gen_one(nd[n].a);
        lab_set(top);
        if (nd[n].b) {
            gen(nd[n].b);
            e1(0x85); e1(0xC0);
            jcc_fwd(0x84, out);
        }
        gen_one(nd[n].j);                              /* the body        */
        lab_set(stp);
        if (nd[n].i) gen_one(nd[n].i);                 /* the step        */
        jmp_fwd(top);
        lab_set(out);
        return;
    }
    case N_RET:
        if (nd[n].a) gen(nd[n].a);
        jmp_fwd(nd[n].i);
        return;
    case N_BREAK:
        jmp_fwd(nd[n].i);
        return;
    case N_CONT:
        jmp_fwd(nd[n].i);
        return;
    }
    cfail("internal: bad statement");
}

/* ---- the top level: globals and functions ------------------------------- */

static int compile_text(void)
{
    lex();
    while (tk != T_EOF) {
        if (!is_type_tok(tk)) return cfail("expected a type");
        int is_char = (tk == T_KCHAR);
        lex();

        for (;;) {
            int stars = 0;
            while (tk == '*') { stars++; lex(); }
            if (tk != T_NAME) return cfail("expected a name");
            char name[32];
            int i = 0;
            while (tkname[i] && i < (int)sizeof name - 1) { name[i] = tkname[i]; i++; }
            name[i] = 0;
            lex();

            if (tk == '(' && !stars) {
                /* a function definition */
                lex();
                if (nfn >= FUNMAX) return cfail("too many functions");
                int f = nfn++;
                fns[f].name = pool_put(name);
                fns[f].params = 0;
                fns[f].code_off = 0;
                fns[f].body = 0;
                fns[f].defined = 0;
                fns[f].end_lab = 0;
                if (streq(name, "main")) fn_main = f;

                sy_base = nsy;                 /* fresh locals, and none  */
                loc_bytes = 0;                 /* carried over (pass 2)   */
                f_end_lab = lab_new();
                int np = 0;
                if (tk != ')') {
                    if (tk == T_KVOID) { lex(); }        /* (void)        */
                    else for (;;) {
                        if (!is_type_tok(tk)) return cfail("expected a type");
                        int pc = (tk == T_KCHAR);
                        lex();
                        int pst = 0;
                        while (tk == '*') { pst++; lex(); }
                        if (tk != T_NAME) return cfail("expected a name");
                        char pname[32];
                        int j = 0;
                        while (tkname[j] && j < (int)sizeof pname - 1) { pname[j] = tkname[j]; j++; }
                        pname[j] = 0;
                        lex();
                        if (nsy >= SYMAX) return cfail("too many names");
                        int q = nsy++;
                        sy[q].name = pool_put(pname);
                        sy[q].is_ptr = (u8)(pst > 0);
                        sy[q].is_char = (u8)pc;
                        sy[q].is_array = 0;
                        sy[q].strinit = 0; sy[q].init = 0; sy[q].has_init = 0;
                        sy[q].scope = 1;
                        sy[q].off = 8 + 4 * np;         /* see the call   */
                        np++;
                        if (tk != ',') break;
                        lex();
                    };
                }
                if (tk != ')') return cfail("expected )");
                lex();
                fns[f].params = np;
                int body = block();
                if (!body) return 0;
                fns[f].body = body;
                fns[f].nloc = loc_bytes;
                fns[f].end_lab = f_end_lab;
                /* the locals stay in the table: the tree refers to them
                 * by slot, and a slot rewritten by the next function
                 * would reach into the wrong frame                  */
                if (tk == ';') lex();          /* stray semicolon         */
                break;                         /* one function, done      */
            }

            /* a global */
            int is_array = 0, count = 1, strinit = -1, initnum = 0, has_init = 0;
            if (tk == '[') {
                lex();
                is_array = 1;
                if (tk == T_NUM) { count = tki; lex(); }
                if (tk == ']') lex();
                if (count < 1) count = 1;
            }
            if (tk == '=') {
                lex();
                has_init = 1;
                if (tk == T_STR) { strinit = tkstr; lex(); }
                else {
                    int neg = 0;
                    if (tk == '-') { neg = 1; lex(); }
                    if (tk != T_NUM) return cfail("globals start as numbers or strings");
                    initnum = neg ? -tki : tki;
                    lex();
                }
            }
            if (nsy >= SYMAX) return cfail("too many names");
            int q = nsy++;
            sy[q].name = pool_put(name);
            sy[q].is_ptr = (u8)(stars > 0);
            sy[q].is_char = (u8)is_char;
            sy[q].is_array = (u8)is_array;
            sy[q].strinit = (u8)(strinit >= 0);
            sy[q].init = strinit >= 0 ? strinit : initnum;
            sy[q].has_init = (u8)has_init;
            sy[q].scope = 0;
            int sz = is_array ? (is_char ? count : count * 4) : 4;
            sy[q].off = gdata_n;
            gdata_n += sz;
            if (gdata_n > GDATAMAX) return cfail("out of global room");
            if (has_init && strinit < 0 && !is_array)
                *(u32 *)(C + CODEMAX + sy[q].off) = (u32)initnum;

            if (tk != ',') break;
            lex();
        }
        if (tk == ';') lex();
    }
    return 1;
}

/* ---- compile, link the patch list, run ---------------------------------- */

static int compile_and_run(void)
{
    u32 t0;
    int rc;

    nd   = (node_t *)api->malloc(NDMAX * (u32)sizeof(node_t));
    sy   = (sym_t *)api->malloc(SYMAX * (u32)sizeof(sym_t));
    fns  = (fn_t  *)api->malloc(FUNMAX * (u32)sizeof(fn_t));
    C    = (u8 *)api->malloc(CODEMAX + GDATAMAX + POOLMAX);
    if (!nd || !sy || !fns || !C) {
        api->puts("cc: not enough memory to compile\n");
        goto done;
    }

    nn = 1; nd[0].op = N_NUM; nd[0].a = nd[0].b = 0;
    nsy = sy_base = nfn = 0;
    fn_main = -1;
    nlab = nfix = npend = dpth = 0;
    gdata_n = pool_n = cp = 0;
    sp = 0; line = 1; err_line = 1;
    poolp = (char *)(C + CODEMAX + GDATAMAX);
    {
        u32 total = CODEMAX + GDATAMAX + POOLMAX;
        for (u32 i = 0; i < total; i++) C[i] = 0;
    }

    if (!compile_text()) goto done;
    if (fn_main < 0) {
        api->puts("cc: nothing to run without main()\n");
        goto done;
    }

    /* the entry: call main, come back */
    e1(0xE8); e32(0);
    pend[npend].pos = cp; pend[npend].fn = fn_main; npend++;
    e1(0xC3);

    for (int f = 0; f < nfn; f++) {
        fns[f].code_off = cp;
        fns[f].defined = 1;
        e1(0x55);                             /* push ebp                */
        e1(0x89); e1(0xE5);                   /* mov ebp,esp             */
        if (fns[f].nloc) {
            e1(0x81); e1(0xEC); e32((u32)fns[f].nloc);
        }
        gen_one(fns[f].body);
        e1(0x31); e1(0xC0);                   /* xor eax,eax (fall off)  */
        lab_set(fns[f].end_lab);
        e1(0xC9);                             /* leave                   */
        e1(0xC3);                             /* ret                     */
    }

    if (nfix >= FIXMAX || npend > PTMAX) goto done;

    /* patch every call to a function, now that they all have addresses */
    for (int i = 0; i < npend; i++) {
        u32 rel = (u32)(fns[pend[i].fn].code_off - pend[i].pos);
        int p = pend[i].pos - 4;
        C[p]   = (u8)rel; C[p+1] = (u8)(rel >> 8);
        C[p+2] = (u8)(rel >> 16); C[p+3] = (u8)(rel >> 24);
    }
    /* and every forward jump */
    for (int i = 0; i < nfix; i++) {
        u32 rel = (u32)(labs[fix[i].lab] - fix[i].pos);
        int p = fix[i].pos - 4;
        C[p]   = (u8)rel; C[p+1] = (u8)(rel >> 8);
        C[p+2] = (u8)(rel >> 16); C[p+3] = (u8)(rel >> 24);
    }
    /* a global pointer initialised with a literal points into the pool,
     * which only now has its final shape                                 */
    for (int i = 0; i < nsy; i++)
        if (sy[i].scope == 0 && sy[i].strinit)
            *(u32 *)(C + CODEMAX + sy[i].off) =
                (u32)(size_t)(poolp + sy[i].init);

    api->puts("\n");
    t0 = api->ticks();
    rc = ((int (*)(void))C)();
    {
        char b[12]; int i = 0;
        u32 v;
        api->puts("\n [program finished, return ");
        v = (u32)rc; if (!v) b[i++] = '0';
        while (v && i < 11) { b[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) api->putc(b[--i]);
        api->puts(", ");
        v = api->ticks() - t0; i = 0; if (!v) b[i++] = '0';
        while (v && i < 11) { b[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) api->putc(b[--i]);
        api->puts(" ticks]\n press any key\n");
    }
    api->getkey();

done:
    if (nd)  api->free(nd);
    if (sy)  api->free(sy);
    if (fns) api->free(fns);
    if (C)   api->free(C);
    nd = 0; sy = 0; fns = 0; C = 0;
    return 0;
}

/* ---- the editor ---------------------------------------------------------- */

static char fname[13] = "UNTITLED.C";
static char kmsg[44] = "";

static void say(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof kmsg - 1) { kmsg[i] = s[i]; i++; }
    kmsg[i] = 0;
}

static int line_of(int pos)
{
    int l = 1;
    for (int i = 0; i < pos; i++)
        if (src[i] == '\n') l++;
    return l;
}

static int line_start(int pos)
{
    while (pos > 0 && src[pos - 1] != '\n') pos--;
    return pos;
}

static void draw_editor(int cpos, int top_line)
{
    int W = api->width(), H = api->height();
    int rows = H - 2;
    int pos = 0, ln = 1;

    api->clear();
    api->setcolor(0, 3);
    api->gotoxy(0, 0);
    api->puts(" cc ");
    api->puts(fname);
    api->puts("  F1 help  F2 save  F5 compile+run ");
    api->setcolor(7, 0);

    while (src[pos] && ln < top_line) {               /* walk to the top  */
        if (src[pos] == '\n') ln++;
        pos++;
    }
    for (int y = 0; y < rows; y++) {
        api->gotoxy(0, 1 + y);
        if (!src[pos]) break;
        int x = 0;
        while (src[pos] && src[pos] != '\n') {
            if (x < W - 1) { api->putc(src[pos]); x++; }
            pos++;
        }
        if (src[pos] == '\n') pos++;
    }

    api->setcolor(0, 3);
    api->gotoxy(0, H - 1);
    api->puts(" row ");
    {
        char b[12]; int i = 0;
        int v = line_of(cpos);
        if (!v) b[i++] = '0';
        while (v && i < 11) { b[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) api->putc(b[--i]);
    }
    api->puts("  run F5  save F2  quit esc ");
    api->puts(kmsg);
    api->puts(" ");
    api->setcolor(7, 0);

    /* the cursor: column of the caret inside its line, on its row     */
    {
        int col = 0, p = cpos;
        while (p > 0 && src[p - 1] != '\n') { p--; col++; }
        int crow = line_of(cpos) - top_line;
        if (crow >= 0 && crow < rows && col < W - 1)
            api->gotoxy(col, 1 + crow);
    }
}

static void editor_help(void)
{
    api->clear();
    api->gotoxy(0, 0);
    api->puts("cc -- a C compiler on the machine itself\n\n");
    api->puts("F5 compiles and runs what is on screen. The language:\n");
    api->puts("  int char void, pointers *, arrays a[10]\n");
    api->puts("  if else while do for break return\n");
    api->puts("  ++ -- && || ! << >> and the whole sum ladder\n");
    api->puts("\nthe machine calls, ready to use:\n");
    api->puts("  putc(c)  puts(s)  getkey()  key()\n");
    api->puts("  ticks()  sleep(ms)  rand(n)  beep(hz,ms)\n");
    api->puts("  clrscr()  gotoxy(x,y)  setcolor(f,b)\n");
    api->puts("  pixel(x,y,c)  fill(x,y,w,h,c)  mouse(&x,&y,&b)\n");
    api->puts("  width()  height()  time(&h,&m,&s)  sound(on)\n");
    api->puts("\nany key goes back\n");
    api->getkey();
}

static void editor(void)
{
    int cpos = 0, top = 1;

    for (;;) {
        int H = api->height();
        draw_editor(cpos, top);
        int k = api->getkey();

        if (k == KEY_ESC) return;
        if (k == KEY_F1) { editor_help(); continue; }
        if (k == KEY_F2) {
            int len = 0;
            while (src[len]) len++;
            if (api->fs_write(fname, src, (u32)len) >= 0)
                say("saved");
            else
                say("save failed");
            continue;
        }
        if (k == KEY_F5) {
            kmsg[0] = 0;
            compile_and_run();
            continue;
        }
        if (k == KEY_UP) {
            cpos = line_start(cpos);
            if (cpos) cpos = line_start(cpos - 1);
        } else if (k == KEY_DOWN) {
            while (src[cpos] && src[cpos] != '\n') cpos++;
            if (src[cpos]) cpos++;
        } else if (k == KEY_LEFT) {
            if (cpos) cpos--;
        } else if (k == KEY_RIGHT) {
            if (src[cpos]) cpos++;
        } else if (k == KEY_HOME) {
            cpos = line_start(cpos);
        } else if (k == KEY_END) {
            while (src[cpos] && src[cpos] != '\n') cpos++;
        } else if (k == KEY_PGUP) {
            for (int i = 0; i < 10; i++) {
                cpos = line_start(cpos);
                if (cpos) cpos = line_start(cpos - 1);
            }
        } else if (k == KEY_PGDN) {
            for (int i = 0; i < 10; i++) {
                while (src[cpos] && src[cpos] != '\n') cpos++;
                if (src[cpos]) cpos++;
            }
        } else if (k == 8) {                          /* backspace       */
            if (cpos) {
                int i = cpos - 1;
                while (src[i]) { src[i] = src[i + 1]; i++; }
                cpos--;
            }
        } else if (k == '\n' || k == '\r') {
            int len = 0;
            while (src[len]) len++;
            if (len < SRCMAX - 2) {
                for (int i = len; i >= cpos; i--) src[i + 1] = src[i];
                src[cpos++] = '\n';
            }
        } else if (k >= 32 && k < 127) {
            int len = 0;
            while (src[len]) len++;
            if (len < SRCMAX - 2) {
                for (int i = len; i >= cpos; i--) src[i + 1] = src[i];
                src[cpos++] = (char)k;
            }
        }

        /* keep the caret on screen */
        {
            int l = line_of(cpos);
            int rows = H - 2;
            if (l < top) top = l;
            if (l >= top + rows) top = l - rows + 1;
        }
    }
}

int app_main(int argc, char **argv)
{
    src = (char *)api->malloc(SRCMAX);
    if (!src) {
        api->puts("cc: not enough memory\n");
        return 1;
    }
    src[0] = 0;

    if (argc > 1 && argv[1][0]) {
        int i = 0;
        while (argv[1][i] && i < (int)sizeof fname - 1) {
            fname[i] = argv[1][i];
            i++;
        }
        fname[i] = 0;
        int n = api->fs_read(argv[1], src, SRCMAX - 1);
        if (n > 0) src[n] = 0;
        else { src[0] = 0; say("new file"); }
    }

    api->cursor(1);
    editor();
    api->clear();
    api->free(src);
    return 0;
}
