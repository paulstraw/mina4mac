// MSVC type name undecoration, as type_info::name does it (__unDName with UNDNAME_TYPE_ONLY): RTTI names
// like ".?AV?$vector@HV?$allocator@H@std@@@std@@" become "class std::vector<int,class std::allocator<int> >".
// Covers what RTTI names use: classes/structs/unions/enums, namespaces (including anonymous ones),
// templates with type and integer arguments, name backreferences, builtin types, pointers, references,
// cv-qualifiers and function (pointer) types. Local classes (named inside a function) and pointers to
// members are not supported: undname fails and type_info::name falls back to the decorated name.
#include "undname.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *names[10], *types[10];  // backreferences: names in qualified names, types in parameter lists
    int nnames, ntypes;
} Scope;

typedef struct {
    const char *s;
    Scope sc;
    int fail;
    char **pool;  // every string allocated, freed at the end
    int npool, cap;
} Dm;

static char *keep(Dm *d, char *s) {
    if (d->npool == d->cap) d->pool = realloc(d->pool, sizeof(char *) * (d->cap = d->cap ? 2 * d->cap : 64));
    return d->pool[d->npool++] = s;
}
static char *fmt(Dm *d, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char *s;
    if (vasprintf(&s, f, ap) < 0) abort();
    va_end(ap);
    return keep(d, s);
}
static char *fail(Dm *d) {
    d->fail = 1;
    return "";
}

static void memo_name(Dm *d, char *n) {
    for (int i = 0; i < d->sc.nnames; i++) if (!strcmp(d->sc.names[i], n)) return;
    if (d->sc.nnames < 10) d->sc.names[d->sc.nnames++] = n;
}

static char *type(Dm *d);

static char *ident(Dm *d) {  // "name@"
    const char *at = strchr(d->s, '@');
    if (!at || at == d->s) return fail(d);
    char *n = fmt(d, "%.*s", (int)(at - d->s), d->s);
    d->s = at + 1;
    return n;
}

static char *number(Dm *d) {  // encoded integer: [?]digit (value + 1) or hex letters A-P ending in '@'
    int neg = *d->s == '?';
    if (neg) d->s++;
    unsigned long long v = 0;
    if (*d->s >= '0' && *d->s <= '9') v = *d->s++ - '0' + 1;
    else {
        while (*d->s >= 'A' && *d->s <= 'P') v = v * 16 + (*d->s++ - 'A');
        if (*d->s++ != '@') return fail(d);
    }
    return fmt(d, neg ? "-%llu" : "%llu", v);
}

static char *close_angle(Dm *d, const char *id, const char *args) {  // "id<args>", with "> >" not ">>"
    size_t n = strlen(args);
    return fmt(d, "%s<%s%s>", id, args, n && args[n - 1] == '>' ? " " : "");
}

static char *template_args(Dm *d) {  // up to and including the closing '@'
    char *out = "";
    while (!d->fail && *d->s != '@') {
        if (!*d->s) return fail(d);
        char *a;
        if (!strncmp(d->s, "$$$V", 4)) { d->s += 4; continue; }  // empty parameter pack
        if (!strncmp(d->s, "$0", 2)) { d->s += 2; a = number(d); }
        else a = type(d);
        out = *out ? fmt(d, "%s,%s", out, a) : a;
    }
    d->s++;
    return out;
}

static char *fragment(Dm *d) {  // one component of a qualified name
    char c = *d->s;
    if (c >= '0' && c <= '9') {
        d->s++;
        return c - '0' < d->sc.nnames ? d->sc.names[c - '0'] : fail(d);
    }
    if (!strncmp(d->s, "?$", 2)) {  // template: its name and arguments get a fresh backreference scope
        d->s += 2;
        Scope outer = d->sc;
        memset(&d->sc, 0, sizeof d->sc);
        char *id = ident(d);
        memo_name(d, id);
        char *args = template_args(d);
        d->sc = outer;
        char *t = close_angle(d, id, args);
        memo_name(d, t);
        return t;
    }
    if (!strncmp(d->s, "?A0x", 4)) {
        ident(d);
        char *n = "`anonymous namespace'";
        memo_name(d, n);
        return n;
    }
    if (c == '?') return fail(d);  // local scopes, special names
    char *n = ident(d);
    memo_name(d, n);
    return n;
}

static char *qualified(Dm *d) {  // fragments innermost first, ending in '@'
    char *out = fragment(d);
    while (!d->fail && *d->s != '@') {
        if (!*d->s) return fail(d);
        out = fmt(d, "%s::%s", fragment(d), out);
    }
    d->s++;
    return out;
}

static const char *cv(char c) {
    switch (c) {
    case 'A': return "";
    case 'B': return " const";
    case 'C': return " volatile";
    case 'D': return " const volatile";
    default: return NULL;
    }
}

static const char *callconv(char c) {
    switch (c) {
    case 'A': return "__cdecl";
    case 'E': return "__thiscall";
    case 'G': return "__stdcall";
    case 'I': return "__fastcall";
    default: return NULL;
    }
}

static char *params(Dm *d) {  // function parameter list, through its terminator
    if (*d->s == 'X') { d->s++; return "void"; }
    char *out = "";
    while (!d->fail && *d->s != '@' && *d->s != 'Z') {
        if (!*d->s) return fail(d);
        char *p;
        if (*d->s >= '0' && *d->s <= '9') {
            int i = *d->s++ - '0';
            p = i < d->sc.ntypes ? d->sc.types[i] : fail(d);
        } else {
            const char *start = d->s;
            p = type(d);
            if (d->s - start > 1 && d->sc.ntypes < 10) d->sc.types[d->sc.ntypes++] = p;
        }
        out = *out ? fmt(d, "%s,%s", out, p) : p;
    }
    if (*d->s == '@') d->s++;
    return out;
}

// Function type after the '6': calling convention, return type, parameters, 'Z' (throw spec).
// `inner` goes where a declarator would: "*" for a pointer, "" for a plain function type.
static char *function(Dm *d, const char *inner) {
    const char *cc = callconv(*d->s++);
    if (!cc) return fail(d);
    if (*d->s == '?') d->s += 2;  // return type storage class (?A), as for class returns
    char *ret = type(d), *ps = params(d);
    if (*d->s++ != 'Z') return fail(d);
    return *inner ? fmt(d, "%s (%s%s)(%s)", ret, cc, inner, ps) : fmt(d, "%s %s(%s)", ret, cc, ps);
}

static char *type(Dm *d) {
    static const char *const BUILTIN[26] = {
        ['C' - 'A'] = "signed char", ['D' - 'A'] = "char", ['E' - 'A'] = "unsigned char", ['F' - 'A'] = "short",
        ['G' - 'A'] = "unsigned short", ['H' - 'A'] = "int", ['I' - 'A'] = "unsigned int", ['J' - 'A'] = "long",
        ['K' - 'A'] = "unsigned long", ['M' - 'A'] = "float", ['N' - 'A'] = "double",
        ['O' - 'A'] = "long double", ['X' - 'A'] = "void",
    };
    if (d->fail) return "";
    char c = *d->s++;
    switch (c) {
    case 'V': return fmt(d, "class %s", qualified(d));
    case 'U': return fmt(d, "struct %s", qualified(d));
    case 'T': return fmt(d, "union %s", qualified(d));
    case 'W':
        if (!*d->s++) return fail(d);  // underlying type digit
        return fmt(d, "enum %s", qualified(d));
    case '_':
        switch (*d->s++) {
        case 'N': return "bool";
        case 'J': return "__int64";
        case 'K': return "unsigned __int64";
        case 'W': return "wchar_t";
        default: return fail(d);
        }
    case 'P': case 'Q': case 'R': case 'S': case 'A': {  // pointer (plain/const/volatile/cv), reference
        const char *self = c == 'A' ? " &" : c == 'Q' ? " * const" : c == 'R' ? " * volatile" : c == 'S' ? " * const volatile" : " *";
        if (*d->s == '6') {
            d->s++;
            return function(d, c == 'A' ? "&" : c == 'Q' ? "*const" : "*");
        }
        const char *q = cv(*d->s++);
        if (!q) return fail(d);
        char *t = type(d);
        size_t n = strlen(t);
        if (c == 'A' && !*q && n && t[n - 1] == '*') self = "&";  // "T *&"
        return fmt(d, "%s%s%s", t, q, self);
    }
    case '$':
        if (!strncmp(d->s, "$C", 2)) {  // cv-qualified type
            d->s += 2;
            const char *q = cv(*d->s++);
            if (!q) return fail(d);
            char *t = type(d);
            return fmt(d, "%s%s", t, q);
        }
        if (!strncmp(d->s, "$A6", 3)) {
            d->s += 3;
            return function(d, "");
        }
        if (!strncmp(d->s, "$Q", 2)) {  // rvalue reference
            d->s += 2;
            const char *q = cv(*d->s++);
            if (!q) return fail(d);
            char *t = type(d);
            return fmt(d, "%s%s &&", t, q);
        }
        return fail(d);
    default:
        if (c >= 'A' && c <= 'Z' && BUILTIN[c - 'A']) return (char *)BUILTIN[c - 'A'];
        return fail(d);
    }
}

char *undname_type(const char *decorated) {
    Dm d = {.s = decorated};
    if (*d.s == '.') d.s++;
    if (strncmp(d.s, "?A", 2)) return NULL;
    d.s += 2;
    char *t = type(&d);
    char *out = d.fail || *d.s ? NULL : strdup(t);
    for (int i = 0; i < d.npool; i++) free(d.pool[i]);
    free(d.pool);
    return out;
}
