#include "common.h"
#include <math.h>
#include <regex.h>

typedef enum { V_NUM, V_STR, V_STRNUM } ValType;

typedef struct {
    ValType type;
    double num;
    char *str;
} Value;

typedef enum {
    E_NUM,
    E_STR,
    E_ERE,
    E_FIELD,
    E_VAR,
    E_ASSIGN,
    E_OPASSIGN,
    E_BINOP,
    E_NEG,
    E_POS,
    E_NOT,
    E_AND,
    E_OR,
    E_MATCH,
    E_CONCAT,
    E_TERNARY,
    E_PREINC,
    E_PREDEC,
    E_POSTINC,
    E_POSTDEC,
    E_CALL
} ExprKind;

typedef struct ExprList {
    struct Expr *e;
    struct ExprList *next;
} ExprList;

typedef struct Expr {
    ExprKind kind;
    double num;
    char *str;
    int op;
    struct Expr *a, *b, *c;
    regex_t *re;
    ExprList *args;
} Expr;

typedef enum {
    S_EXPR,
    S_PRINT,
    S_PRINTF,
    S_IF,
    S_WHILE,
    S_FOR,
    S_BLOCK,
    S_NEXT,
    S_EXIT,
    S_BREAK,
    S_CONTINUE
} StmtKind;

typedef struct Stmt {
    StmtKind kind;
    Expr *cond;
    struct Stmt *init, *post, *body, *els;
    ExprList *args;
    struct Stmt *next;
} Stmt;

typedef enum { PAT_ALWAYS, PAT_BEGIN, PAT_END, PAT_EXPR } PatKind;

typedef struct Item {
    PatKind kind;
    Expr *expr;
    Stmt *action;
    struct Item *next;
} Item;

typedef enum {
    T_EOF,
    T_NUM,
    T_STR,
    T_ERE,
    T_IDENT,
    T_LBRACE,
    T_RBRACE,
    T_LPAREN,
    T_RPAREN,
    T_SEMI,
    T_COMMA,
    T_DOLLAR,
    T_ASSIGN,
    T_ADDA,
    T_SUBA,
    T_MULA,
    T_DIVA,
    T_MODA,
    T_OROR,
    T_ANDAND,
    T_NOT,
    T_LT,
    T_LE,
    T_GT,
    T_GE,
    T_EQ,
    T_NE,
    T_MATCH,
    T_NOTMATCH,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT,
    T_CARET,
    T_INC,
    T_DEC,
    T_QMARK,
    T_COLON,
    T_BEGIN,
    T_END,
    T_IF,
    T_ELSE,
    T_WHILE,
    T_FOR,
    T_PRINT,
    T_PRINTF,
    T_NEXT,
    T_EXIT,
    T_BREAK,
    T_CONTINUE
} TokType;

typedef struct {
    const char *src;
    size_t pos;
    int expect_operand;
    TokType type;
    char *text;
    double num;
} Lexer;

static Lexer L;

static void lex_error(const char *msg) {
    fprintf(stderr, "awk: %s\n", msg);
    exit(2);
}

static int kw_lookup(const char *s, TokType *t) {
    static const struct {
        const char *name;
        TokType t;
    } kws[] = {
        { "BEGIN", T_BEGIN }, { "END", T_END },   { "if", T_IF },       { "else", T_ELSE },
        { "while", T_WHILE }, { "for", T_FOR },   { "print", T_PRINT }, { "printf", T_PRINTF },
        { "next", T_NEXT },   { "exit", T_EXIT }, { "break", T_BREAK }, { "continue", T_CONTINUE },
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (strcmp(s, kws[i].name) == 0) {
            *t = kws[i].t;
            return 1;
        }
    return 0;
}

static void lex_next(void) {
    free(L.text);
    L.text = NULL;
    for (;;) {
        char c = L.src[L.pos];
        if (c == '\\' && L.src[L.pos + 1] == '\n') {
            L.pos += 2;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            L.pos++;
            continue;
        }
        if (c == '#') {
            while (L.src[L.pos] && L.src[L.pos] != '\n') L.pos++;
            continue;
        }
        break;
    }
    char c = L.src[L.pos];
    if (c == 0) {
        L.type = T_EOF;
        return;
    }
    if (isdigit((unsigned char) c) || (c == '.' && isdigit((unsigned char) L.src[L.pos + 1]))) {
        char *end;
        L.num = strtod(L.src + L.pos, &end);
        L.pos = (size_t) (end - L.src);
        L.type = T_NUM;
        L.expect_operand = 0;
        return;
    }
    if (c == '"') {
        L.pos++;
        size_t cap = 32, len = 0;
        char *buf = malloc(cap);
        while (L.src[L.pos] && L.src[L.pos] != '"') {
            char ch = L.src[L.pos++];
            if (ch == '\\' && L.src[L.pos]) {
                char e = L.src[L.pos++];
                switch (e) {
                case 'n':
                    ch = '\n';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case '"':
                    ch = '"';
                    break;
                case '/':
                    ch = '/';
                    break;
                default:
                    ch = e;
                    break;
                }
            }
            if (len + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = ch;
        }
        if (L.src[L.pos] == '"') L.pos++;
        buf[len] = 0;
        L.text = buf;
        L.type = T_STR;
        L.expect_operand = 0;
        return;
    }
    if (c == '/' && L.expect_operand) {
        L.pos++;
        size_t cap = 32, len = 0;
        char *buf = malloc(cap);
        while (L.src[L.pos] && L.src[L.pos] != '/') {
            char ch = L.src[L.pos++];
            if (ch == '\\' && L.src[L.pos]) {
                buf = (len + 2 >= cap) ? (cap *= 2, realloc(buf, cap)) : buf;
                buf[len++] = ch;
                ch = L.src[L.pos++];
            }
            if (len + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = ch;
        }
        if (L.src[L.pos] == '/') L.pos++;
        buf[len] = 0;
        L.text = buf;
        L.type = T_ERE;
        L.expect_operand = 0;
        return;
    }
    if (isalpha((unsigned char) c) || c == '_') {
        size_t start = L.pos;
        while (isalnum((unsigned char) L.src[L.pos]) || L.src[L.pos] == '_') L.pos++;
        size_t len = L.pos - start;
        char *buf = malloc(len + 1);
        memcpy(buf, L.src + start, len);
        buf[len] = 0;
        TokType kt;
        if (kw_lookup(buf, &kt)) {
            free(buf);
            L.type = kt;
            L.expect_operand = 1;
            return;
        }
        L.text = buf;
        L.type = T_IDENT;
        L.expect_operand = 0;
        return;
    }
    char c2 = L.src[L.pos + 1];
    L.pos++;
    switch (c) {
    case '{':
        L.type = T_LBRACE;
        L.expect_operand = 1;
        return;
    case '}':
        L.type = T_RBRACE;
        L.expect_operand = 1;
        return;
    case '(':
        L.type = T_LPAREN;
        L.expect_operand = 1;
        return;
    case ')':
        L.type = T_RPAREN;
        L.expect_operand = 0;
        return;
    case ';':
        L.type = T_SEMI;
        L.expect_operand = 1;
        return;
    case ',':
        L.type = T_COMMA;
        L.expect_operand = 1;
        return;
    case '$':
        L.type = T_DOLLAR;
        L.expect_operand = 1;
        return;
    case '?':
        L.type = T_QMARK;
        L.expect_operand = 1;
        return;
    case ':':
        L.type = T_COLON;
        L.expect_operand = 1;
        return;
    case '^':
        L.type = T_CARET;
        L.expect_operand = 1;
        return;
    case '~':
        L.type = T_MATCH;
        L.expect_operand = 1;
        return;
    case '+':
        if (c2 == '+') {
            L.pos++;
            L.type = T_INC;
            L.expect_operand = 1;
            return;
        }
        if (c2 == '=') {
            L.pos++;
            L.type = T_ADDA;
            L.expect_operand = 1;
            return;
        }
        L.type = T_PLUS;
        L.expect_operand = 1;
        return;
    case '-':
        if (c2 == '-') {
            L.pos++;
            L.type = T_DEC;
            L.expect_operand = 1;
            return;
        }
        if (c2 == '=') {
            L.pos++;
            L.type = T_SUBA;
            L.expect_operand = 1;
            return;
        }
        L.type = T_MINUS;
        L.expect_operand = 1;
        return;
    case '*':
        if (c2 == '=') {
            L.pos++;
            L.type = T_MULA;
            L.expect_operand = 1;
            return;
        }
        L.type = T_STAR;
        L.expect_operand = 1;
        return;
    case '/':
        if (c2 == '=') {
            L.pos++;
            L.type = T_DIVA;
            L.expect_operand = 1;
            return;
        }
        L.type = T_SLASH;
        L.expect_operand = 1;
        return;
    case '%':
        if (c2 == '=') {
            L.pos++;
            L.type = T_MODA;
            L.expect_operand = 1;
            return;
        }
        L.type = T_PERCENT;
        L.expect_operand = 1;
        return;
    case '=':
        if (c2 == '=') {
            L.pos++;
            L.type = T_EQ;
            L.expect_operand = 1;
            return;
        }
        L.type = T_ASSIGN;
        L.expect_operand = 1;
        return;
    case '!':
        if (c2 == '=') {
            L.pos++;
            L.type = T_NE;
            L.expect_operand = 1;
            return;
        }
        if (c2 == '~') {
            L.pos++;
            L.type = T_NOTMATCH;
            L.expect_operand = 1;
            return;
        }
        L.type = T_NOT;
        L.expect_operand = 1;
        return;
    case '<':
        if (c2 == '=') {
            L.pos++;
            L.type = T_LE;
            L.expect_operand = 1;
            return;
        }
        L.type = T_LT;
        L.expect_operand = 1;
        return;
    case '>':
        if (c2 == '=') {
            L.pos++;
            L.type = T_GE;
            L.expect_operand = 1;
            return;
        }
        L.type = T_GT;
        L.expect_operand = 1;
        return;
    case '&':
        if (c2 == '&') {
            L.pos++;
            L.type = T_ANDAND;
            L.expect_operand = 1;
            return;
        }
        lex_error("stray '&'");
        return;
    case '|':
        if (c2 == '|') {
            L.pos++;
            L.type = T_OROR;
            L.expect_operand = 1;
            return;
        }
        lex_error("stray '|'");
        return;
    default:
        lex_error("unexpected character in program");
    }
}

static Expr *new_expr(ExprKind k) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = k;
    return e;
}

static Stmt *new_stmt(StmtKind k) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = k;
    return s;
}

static void expect(TokType t, const char *what) {
    if (L.type != t) {
        fprintf(stderr, "awk: expected %s\n", what);
        exit(2);
    }
    lex_next();
}

static Expr *parse_expr(void);
static Expr *parse_ternary(void);
static Stmt *parse_stmt(void);
static Stmt *parse_block(void);

static ExprList *parse_expr_list(void) {
    ExprList *head = NULL, **tail = &head;
    if (L.type == T_RPAREN || L.type == T_SEMI || L.type == T_RBRACE || L.type == T_EOF)
        return NULL;
    Expr *first;
    if (L.type == T_LPAREN) {
        size_t save_pos = L.pos - 1;
        lex_next();
        first = parse_ternary();
        if (L.type == T_COMMA) {
            ExprList *n = calloc(1, sizeof(ExprList));
            n->e = first;
            *tail = n;
            tail = &n->next;
            while (L.type == T_COMMA) {
                lex_next();
                n = calloc(1, sizeof(ExprList));
                n->e = parse_ternary();
                *tail = n;
                tail = &n->next;
            }
            expect(T_RPAREN, "')'");
            return head;
        }
        expect(T_RPAREN, "')'");
        if (L.type == T_SEMI || L.type == T_RBRACE || L.type == T_EOF) {
            ExprList *n = calloc(1, sizeof(ExprList));
            n->e = first;
            *tail = n;
            tail = &n->next;
            return head;
        }
        L.pos = save_pos;
        L.expect_operand = 1;
        lex_next();
    }
    for (;;) {
        ExprList *n = calloc(1, sizeof(ExprList));
        n->e = parse_ternary();
        *tail = n;
        tail = &n->next;
        if (L.type != T_COMMA) break;
        lex_next();
    }
    return head;
}

static int is_builtin(const char *name) {
    static const char *names[] = { "length", "substr", "index", "sprintf", "toupper", "tolower",
                                   "int",    "sin",    "cos",   "atan2",   "exp",     "log",
                                   "sqrt",   "rand",   "srand", NULL };
    for (int i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static Expr *parse_primary(void) {
    Expr *e;
    switch (L.type) {
    case T_NUM:
        e = new_expr(E_NUM);
        e->num = L.num;
        lex_next();
        return e;
    case T_STR:
        e = new_expr(E_STR);
        e->str = L.text;
        L.text = NULL;
        lex_next();
        return e;
    case T_ERE:
        e = new_expr(E_ERE);
        e->str = L.text;
        L.text = NULL;
        e->re = malloc(sizeof(regex_t));
        if (regcomp(e->re, e->str, REG_EXTENDED | REG_NOSUB) != 0)
            lex_error("bad regular expression");
        lex_next();
        return e;
    case T_DOLLAR:
        lex_next();
        e = new_expr(E_FIELD);
        e->a = parse_primary();
        return e;
    case T_LPAREN: {
        lex_next();
        e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
    case T_NOT:
        lex_next();
        e = new_expr(E_NOT);
        e->a = parse_primary();
        return e;
    case T_MINUS:
        lex_next();
        e = new_expr(E_NEG);
        e->a = parse_primary();
        return e;
    case T_PLUS:
        lex_next();
        e = new_expr(E_POS);
        e->a = parse_primary();
        return e;
    case T_INC:
    case T_DEC: {
        int isinc = (L.type == T_INC);
        lex_next();
        e = new_expr(isinc ? E_PREINC : E_PREDEC);
        e->a = parse_primary();
        return e;
    }
    case T_IDENT: {
        char *name = L.text;
        L.text = NULL;
        lex_next();
        if (L.type == T_LPAREN && is_builtin(name)) {
            lex_next();
            e = new_expr(E_CALL);
            e->str = name;
            e->args = parse_expr_list();
            expect(T_RPAREN, "')'");
            return e;
        }
        e = new_expr(E_VAR);
        e->str = name;
        if (L.type == T_INC) {
            lex_next();
            Expr *p = new_expr(E_POSTINC);
            p->a = e;
            return p;
        }
        if (L.type == T_DEC) {
            lex_next();
            Expr *p = new_expr(E_POSTDEC);
            p->a = e;
            return p;
        }
        return e;
    }
    default:
        fprintf(stderr, "awk: unexpected token in expression\n");
        exit(2);
    }
}

static Expr *parse_pow(void) {
    Expr *e = parse_primary();
    if (L.type == T_CARET) {
        lex_next();
        Expr *b = new_expr(E_BINOP);
        b->op = '^';
        b->a = e;
        b->b = parse_pow();
        return b;
    }
    return e;
}

static Expr *parse_mul(void) {
    Expr *e = parse_pow();
    while (L.type == T_STAR || L.type == T_SLASH || L.type == T_PERCENT) {
        int op = L.type == T_STAR ? '*' : L.type == T_SLASH ? '/' : '%';
        lex_next();
        Expr *b = new_expr(E_BINOP);
        b->op = op;
        b->a = e;
        b->b = parse_pow();
        e = b;
    }
    return e;
}

static Expr *parse_add(void) {
    Expr *e = parse_mul();
    while (L.type == T_PLUS || L.type == T_MINUS) {
        int op = L.type == T_PLUS ? '+' : '-';
        lex_next();
        Expr *b = new_expr(E_BINOP);
        b->op = op;
        b->a = e;
        b->b = parse_mul();
        e = b;
    }
    return e;
}

static int starts_primary(void) {
    switch (L.type) {
    case T_NUM:
    case T_STR:
    case T_ERE:
    case T_IDENT:
    case T_DOLLAR:
    case T_LPAREN:
    case T_NOT:
        return 1;
    default:
        return 0;
    }
}

static Expr *parse_concat(void) {
    Expr *e = parse_add();
    while (starts_primary()) {
        Expr *b = new_expr(E_CONCAT);
        b->a = e;
        b->b = parse_add();
        e = b;
    }
    return e;
}

static Expr *parse_rel(void) {
    Expr *e = parse_concat();
    TokType t = L.type;
    if (t == T_LT || t == T_LE || t == T_GT || t == T_GE || t == T_EQ || t == T_NE) {
        lex_next();
        Expr *b = new_expr(E_BINOP);
        b->op = t;
        b->a = e;
        b->b = parse_concat();
        return b;
    }
    return e;
}

static Expr *parse_match(void) {
    Expr *e = parse_rel();
    while (L.type == T_MATCH || L.type == T_NOTMATCH) {
        int neg = (L.type == T_NOTMATCH);
        lex_next();
        Expr *b = new_expr(E_MATCH);
        b->op = neg;
        b->a = e;
        b->b = parse_rel();
        e = b;
    }
    return e;
}

static Expr *parse_and(void) {
    Expr *e = parse_match();
    while (L.type == T_ANDAND) {
        lex_next();
        Expr *b = new_expr(E_AND);
        b->a = e;
        b->b = parse_match();
        e = b;
    }
    return e;
}

static Expr *parse_or(void) {
    Expr *e = parse_and();
    while (L.type == T_OROR) {
        lex_next();
        Expr *b = new_expr(E_OR);
        b->a = e;
        b->b = parse_and();
        e = b;
    }
    return e;
}

static Expr *parse_ternary(void) {
    Expr *e = parse_or();
    if (L.type == T_QMARK) {
        lex_next();
        Expr *t = new_expr(E_TERNARY);
        t->a = e;
        t->b = parse_ternary();
        expect(T_COLON, "':'");
        t->c = parse_ternary();
        return t;
    }
    return e;
}

static int is_lvalue(Expr *e) { return e->kind == E_VAR || e->kind == E_FIELD; }

static Expr *parse_expr(void) {
    Expr *e = parse_ternary();
    TokType t = L.type;
    if ((t == T_ASSIGN || t == T_ADDA || t == T_SUBA || t == T_MULA || t == T_DIVA ||
         t == T_MODA) &&
        is_lvalue(e)) {
        lex_next();
        Expr *rhs = parse_expr();
        Expr *a = new_expr(t == T_ASSIGN ? E_ASSIGN : E_OPASSIGN);
        a->op = t;
        a->a = e;
        a->b = rhs;
        return a;
    }
    return e;
}

static void skip_semis(void) {
    while (L.type == T_SEMI) lex_next();
}

static Stmt *parse_simple_or_block(void) {
    skip_semis();
    if (L.type == T_LBRACE) return parse_block();
    return parse_stmt();
}

static Stmt *parse_stmt(void) {
    switch (L.type) {
    case T_LBRACE:
        return parse_block();
    case T_IF: {
        lex_next();
        expect(T_LPAREN, "'('");
        Stmt *s = new_stmt(S_IF);
        s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        s->body = parse_simple_or_block();
        skip_semis();
        if (L.type == T_ELSE) {
            lex_next();
            s->els = parse_simple_or_block();
        }
        return s;
    }
    case T_WHILE: {
        lex_next();
        expect(T_LPAREN, "'('");
        Stmt *s = new_stmt(S_WHILE);
        s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        s->body = parse_simple_or_block();
        return s;
    }
    case T_FOR: {
        lex_next();
        expect(T_LPAREN, "'('");
        Stmt *s = new_stmt(S_FOR);
        if (L.type != T_SEMI) s->init = new_stmt(S_EXPR), s->init->cond = parse_expr();
        expect(T_SEMI, "';'");
        if (L.type != T_SEMI) s->cond = parse_expr();
        expect(T_SEMI, "';'");
        if (L.type != T_RPAREN) s->post = new_stmt(S_EXPR), s->post->cond = parse_expr();
        expect(T_RPAREN, "')'");
        s->body = parse_simple_or_block();
        return s;
    }
    case T_PRINT: {
        lex_next();
        Stmt *s = new_stmt(S_PRINT);
        s->args = parse_expr_list();
        return s;
    }
    case T_PRINTF: {
        lex_next();
        Stmt *s = new_stmt(S_PRINTF);
        s->args = parse_expr_list();
        return s;
    }
    case T_NEXT:
        lex_next();
        return new_stmt(S_NEXT);
    case T_BREAK:
        lex_next();
        return new_stmt(S_BREAK);
    case T_CONTINUE:
        lex_next();
        return new_stmt(S_CONTINUE);
    case T_EXIT: {
        lex_next();
        Stmt *s = new_stmt(S_EXIT);
        if (L.type != T_SEMI && L.type != T_RBRACE && L.type != T_EOF) s->cond = parse_expr();
        return s;
    }
    case T_SEMI:
        return new_stmt(S_BLOCK);
    default: {
        Stmt *s = new_stmt(S_EXPR);
        s->cond = parse_expr();
        return s;
    }
    }
}

static Stmt *parse_block(void) {
    expect(T_LBRACE, "'{'");
    Stmt *head = NULL, **tail = &head;
    skip_semis();
    while (L.type != T_RBRACE && L.type != T_EOF) {
        Stmt *s = parse_stmt();
        *tail = s;
        tail = &s->next;
        skip_semis();
    }
    expect(T_RBRACE, "'}'");
    Stmt *blk = new_stmt(S_BLOCK);
    blk->body = head;
    return blk;
}

static Item *parse_program(const char *text) {
    L.src = text;
    L.pos = 0;
    L.expect_operand = 1;
    L.text = NULL;
    lex_next();
    Item *head = NULL, **tail = &head;
    skip_semis();
    while (L.type != T_EOF) {
        Item *it = calloc(1, sizeof(Item));
        if (L.type == T_BEGIN) {
            it->kind = PAT_BEGIN;
            lex_next();
            it->action = parse_block();
        } else if (L.type == T_END) {
            it->kind = PAT_END;
            lex_next();
            it->action = parse_block();
        } else if (L.type == T_LBRACE) {
            it->kind = PAT_ALWAYS;
            it->action = parse_block();
        } else {
            it->kind = PAT_EXPR;
            it->expr = parse_expr();
            if (L.type == T_LBRACE) it->action = parse_block();
        }
        *tail = it;
        tail = &it->next;
        skip_semis();
    }
    return head;
}

typedef struct VarNode {
    char *name;
    Value v;
    struct VarNode *next;
} VarNode;

#define VARTAB_SIZE 64
static VarNode *vartab[VARTAB_SIZE];

static char **fields;
static int nf, fields_cap;
static char *record;
static char fs[64] = " ";
static char ofs_val[64] = " ";
static char ors_val[64] = "\n";
static long nr_val;
static char *filename_val = (char *) "";
static unsigned int rand_seed = 1;

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char) *s++;
    return h;
}

static VarNode *find_var(const char *name, int create) {
    unsigned long h = hash_str(name) % VARTAB_SIZE;
    for (VarNode *n = vartab[h]; n; n = n->next)
        if (strcmp(n->name, name) == 0) return n;
    if (!create) return NULL;
    VarNode *n = calloc(1, sizeof(VarNode));
    n->name = strdup(name);
    n->v.type = V_STRNUM;
    n->v.str = strdup("");
    n->v.num = 0;
    n->next = vartab[h];
    vartab[h] = n;
    return n;
}

static int looks_numeric(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (!*s) return 0;
    char *end;
    strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    return *end == 0;
}

static Value mknum(double d) {
    Value v;
    v.type = V_NUM;
    v.num = d;
    v.str = NULL;
    return v;
}

static Value mkstr(const char *s) {
    Value v;
    v.type = V_STR;
    v.str = strdup(s ? s : "");
    v.num = 0;
    return v;
}

static Value mkstrnum(const char *s) {
    if (looks_numeric(s)) {
        Value v;
        v.type = V_STRNUM;
        v.str = strdup(s);
        v.num = strtod(s, NULL);
        return v;
    }
    return mkstr(s);
}

static double to_num(Value v) {
    if (v.type == V_NUM) return v.num;
    return strtod(v.str ? v.str : "", NULL);
}

static const char *fmt_num(double d, char *buf, size_t bufsz) {
    if (d == (long long) d && fabs(d) < 1e15)
        snprintf(buf, bufsz, "%lld", (long long) d);
    else
        snprintf(buf, bufsz, "%.6g", d);
    return buf;
}

static const char *to_str(Value v, char *buf, size_t bufsz) {
    if (v.type != V_NUM) return v.str ? v.str : "";
    return fmt_num(v.num, buf, bufsz);
}

static int truthy(Value v) {
    if (v.type == V_STR) return v.str && v.str[0] != 0;
    return to_num(v) != 0;
}

static int compare(Value a, Value b) {
    if (a.type != V_STR && b.type != V_STR) {
        double x = to_num(a), y = to_num(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    char bufa[64], bufb[64];
    return strcmp(to_str(a, bufa, sizeof bufa), to_str(b, bufb, sizeof bufb));
}

static void rebuild_record(void) {
    size_t len = 1;
    for (int i = 0; i < nf; i++) len += strlen(fields[i]) + strlen(ofs_val);
    char *buf = malloc(len);
    buf[0] = 0;
    for (int i = 0; i < nf; i++) {
        if (i > 0) strcat(buf, ofs_val);
        strcat(buf, fields[i]);
    }
    free(record);
    record = buf;
}

static void ensure_field_cap(int n) {
    if (n <= fields_cap) return;
    int newcap = fields_cap ? fields_cap : 8;
    while (newcap < n) newcap *= 2;
    fields = realloc(fields, sizeof(char *) * (size_t) newcap);
    for (int i = fields_cap; i < newcap; i++) fields[i] = NULL;
    fields_cap = newcap;
}

static void split_record(void) {
    for (int i = 0; i < nf; i++) {
        free(fields[i]);
        fields[i] = NULL;
    }
    nf = 0;
    const char *p = record;
    if (strcmp(fs, " ") == 0) {
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            ensure_field_cap(nf + 1);
            fields[nf] = malloc((size_t) (p - start) + 1);
            memcpy(fields[nf], start, (size_t) (p - start));
            fields[nf][p - start] = 0;
            nf++;
        }
        return;
    }
    if (strlen(fs) == 1) {
        char sep = fs[0];
        if (*p == 0) return;
        const char *start = p;
        for (;; p++) {
            if (*p == sep || *p == 0) {
                ensure_field_cap(nf + 1);
                fields[nf] = malloc((size_t) (p - start) + 1);
                memcpy(fields[nf], start, (size_t) (p - start));
                fields[nf][p - start] = 0;
                nf++;
                if (*p == 0) break;
                start = p + 1;
            }
        }
        return;
    }
    regex_t re;
    if (regcomp(&re, fs, REG_EXTENDED) != 0) {
        ensure_field_cap(1);
        fields[0] = strdup(record);
        nf = 1;
        return;
    }
    if (*p == 0) {
        regfree(&re);
        return;
    }
    const char *start = p;
    regmatch_t m;
    while (*p && regexec(&re, p, 1, &m, 0) == 0 && m.rm_so != m.rm_eo) {
        ensure_field_cap(nf + 1);
        fields[nf] = malloc((size_t) (p + m.rm_so - start) + 1);
        memcpy(fields[nf], start, (size_t) (p + m.rm_so - start));
        fields[nf][p + m.rm_so - start] = 0;
        nf++;
        p += m.rm_eo;
        start = p;
    }
    ensure_field_cap(nf + 1);
    fields[nf] = strdup(start);
    nf++;
    regfree(&re);
}

static void set_record(const char *s) {
    free(record);
    record = strdup(s);
    split_record();
}

static const char *get_field(int n) {
    if (n == 0) return record ? record : "";
    if (n < 0 || n > nf) return "";
    return fields[n - 1];
}

static void set_field(int n, const char *s) {
    if (n == 0) {
        set_record(s);
        return;
    }
    if (n < 0) return;
    if (n > nf) {
        ensure_field_cap(n);
        for (int i = nf; i < n; i++) fields[i] = strdup("");
        nf = n;
    }
    free(fields[n - 1]);
    fields[n - 1] = strdup(s);
    rebuild_record();
}

static void set_nf(int n) {
    if (n < 0) n = 0;
    if (n < nf) {
        for (int i = n; i < nf; i++) {
            free(fields[i]);
            fields[i] = NULL;
        }
        nf = n;
    } else if (n > nf) {
        ensure_field_cap(n);
        for (int i = nf; i < n; i++) fields[i] = strdup("");
        nf = n;
    }
    rebuild_record();
}

static Value get_special(const char *name, int *found) {
    *found = 1;
    if (strcmp(name, "NF") == 0) return mknum(nf);
    if (strcmp(name, "NR") == 0) return mknum((double) nr_val);
    if (strcmp(name, "FS") == 0) return mkstr(fs);
    if (strcmp(name, "OFS") == 0) return mkstr(ofs_val);
    if (strcmp(name, "ORS") == 0) return mkstr(ors_val);
    if (strcmp(name, "FILENAME") == 0) return mkstr(filename_val);
    *found = 0;
    return mknum(0);
}

static int set_special(const char *name, Value v) {
    char buf[64];
    if (strcmp(name, "NF") == 0) {
        set_nf((int) to_num(v));
        return 1;
    }
    if (strcmp(name, "FS") == 0) {
        snprintf(fs, sizeof fs, "%s", to_str(v, buf, sizeof buf));
        return 1;
    }
    if (strcmp(name, "OFS") == 0) {
        snprintf(ofs_val, sizeof ofs_val, "%s", to_str(v, buf, sizeof buf));
        return 1;
    }
    if (strcmp(name, "ORS") == 0) {
        snprintf(ors_val, sizeof ors_val, "%s", to_str(v, buf, sizeof buf));
        return 1;
    }
    if (strcmp(name, "NR") == 0) {
        nr_val = (long) to_num(v);
        return 1;
    }
    return 0;
}

static Value get_var(const char *name) {
    int found;
    Value v = get_special(name, &found);
    if (found) return v;
    VarNode *n = find_var(name, 1);
    if (n->v.type == V_NUM) return mknum(n->v.num);
    return n->v.type == V_STRNUM ? mkstrnum(n->v.str) : mkstr(n->v.str);
}

static void set_var(const char *name, Value v) {
    if (set_special(name, v)) return;
    VarNode *n = find_var(name, 1);
    free(n->v.str);
    n->v = v;
    if (v.str) n->v.str = strdup(v.str);
}

typedef enum { EX_NORMAL, EX_BREAK, EX_CONTINUE, EX_NEXT, EX_EXIT } ExecSig;

static int exit_code;

static Value eval(Expr *e);

static void unescape_inplace(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '\\' && r[1]) {
            char e = r[1];
            switch (e) {
            case 'n':
                *w++ = '\n';
                r += 2;
                break;
            case 't':
                *w++ = '\t';
                r += 2;
                break;
            case '\\':
                *w++ = '\\';
                r += 2;
                break;
            default:
                *w++ = *r++;
                break;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void do_printf(FILE *out, const char *fmt, ExprList *args) {
    ExprList *a = args;
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            fputc(*p++, out);
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            fputc('%', out);
            p++;
            continue;
        }
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ 0#", *p) && sl < sizeof spec - 2) spec[sl++] = *p++;
        while (isdigit((unsigned char) *p) && sl < sizeof spec - 2) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            while (isdigit((unsigned char) *p) && sl < sizeof spec - 2) spec[sl++] = *p++;
        }
        if (!*p) {
            fwrite(start, 1, (size_t) (p - start), out);
            break;
        }
        char conv = *p++;
        spec[sl] = 0;
        Value v = a ? eval(a->e) : mknum(0);
        if (a) a = a->next;
        char obuf[512];
        switch (conv) {
        case 'd':
        case 'i': {
            char s2[70];
            snprintf(s2, sizeof s2, "%sld", spec);
            snprintf(obuf, sizeof obuf, s2, (long) to_num(v));
            fputs(obuf, out);
            break;
        }
        case 'o':
        case 'x':
        case 'X':
        case 'u': {
            char s2[70];
            snprintf(s2, sizeof s2, "%sl%c", spec, conv);
            snprintf(obuf, sizeof obuf, s2, (unsigned long) to_num(v));
            fputs(obuf, out);
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            char s2[70];
            snprintf(s2, sizeof s2, "%s%c", spec, conv);
            snprintf(obuf, sizeof obuf, s2, to_num(v));
            fputs(obuf, out);
            break;
        }
        case 'c': {
            char valbuf[64];
            const char *sv = to_str(v, valbuf, sizeof valbuf);
            char s2[70];
            snprintf(s2, sizeof s2, "%sc", spec);
            snprintf(obuf, sizeof obuf, s2, sv[0] ? sv[0] : (char) (long) to_num(v));
            fputs(obuf, out);
            break;
        }
        case 's':
        default: {
            char valbuf[512];
            const char *sv = to_str(v, valbuf, sizeof valbuf);
            char s2[70];
            snprintf(s2, sizeof s2, "%ss", spec);
            char big[4096];
            snprintf(big, sizeof big, s2, sv);
            fputs(big, out);
            break;
        }
        }
    }
}

static char *sprintf_to_str(const char *fmt, ExprList *args) {
    char tmp[] = "/tmp/kx_awk_sprintfXXXXXX";
    (void) tmp;
    FILE *f = tmpfile();
    if (!f) return strdup("");
    do_printf(f, fmt, args);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc((size_t) len + 1);
    size_t rd = fread(buf, 1, (size_t) len, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

static Value call_builtin(const char *name, ExprList *args) {
    char buf[512];
    if (strcmp(name, "length") == 0) {
        if (!args) return mknum(strlen(get_field(0)));
        Value v = eval(args->e);
        return mknum((double) strlen(to_str(v, buf, sizeof buf)));
    }
    if (strcmp(name, "substr") == 0) {
        Value sv = eval(args->e);
        const char *s = to_str(sv, buf, sizeof buf);
        long slen = (long) strlen(s);
        long m = args->next ? (long) to_num(eval(args->next->e)) : 1;
        long n =
            (args->next && args->next->next) ? (long) to_num(eval(args->next->next->e)) : LONG_MAX;
        if (m < 1) {
            n += (m - 1);
            m = 1;
        }
        if (n < 0) n = 0;
        if (m > slen) return mkstr("");
        long avail = slen - m + 1;
        if (n > avail) n = avail;
        char *r = malloc((size_t) n + 1);
        memcpy(r, s + m - 1, (size_t) n);
        r[n] = 0;
        Value ret = mkstr(r);
        free(r);
        return ret;
    }
    if (strcmp(name, "index") == 0) {
        char b1[512], b2[512];
        Value a = eval(args->e), b = eval(args->next->e);
        const char *hay = to_str(a, b1, sizeof b1), *needle = to_str(b, b2, sizeof b2);
        const char *p = strstr(hay, needle);
        return mknum(p ? (double) (p - hay + 1) : 0);
    }
    if (strcmp(name, "sprintf") == 0) {
        Value fv = eval(args->e);
        char *r = sprintf_to_str(to_str(fv, buf, sizeof buf), args->next);
        Value ret = mkstr(r);
        free(r);
        return ret;
    }
    if (strcmp(name, "toupper") == 0 || strcmp(name, "tolower") == 0) {
        Value sv = eval(args->e);
        const char *s = to_str(sv, buf, sizeof buf);
        char *r = strdup(s);
        for (char *p = r; *p; p++)
            *p = (char) (strcmp(name, "toupper") == 0 ? toupper((unsigned char) *p) :
                                                        tolower((unsigned char) *p));
        Value ret = mkstr(r);
        free(r);
        return ret;
    }
    if (strcmp(name, "int") == 0) return mknum((double) (long) to_num(eval(args->e)));
    if (strcmp(name, "sin") == 0) return mknum(sin(to_num(eval(args->e))));
    if (strcmp(name, "cos") == 0) return mknum(cos(to_num(eval(args->e))));
    if (strcmp(name, "exp") == 0) return mknum(exp(to_num(eval(args->e))));
    if (strcmp(name, "log") == 0) return mknum(log(to_num(eval(args->e))));
    if (strcmp(name, "sqrt") == 0) return mknum(sqrt(to_num(eval(args->e))));
    if (strcmp(name, "atan2") == 0)
        return mknum(atan2(to_num(eval(args->e)), to_num(eval(args->next->e))));
    if (strcmp(name, "rand") == 0)
        return mknum((double) rand_r(&rand_seed) / ((double) RAND_MAX + 1));
    if (strcmp(name, "srand") == 0) {
        unsigned int prev = rand_seed;
        rand_seed = args ? (unsigned int) to_num(eval(args->e)) : (unsigned int) time(NULL);
        return mknum(prev);
    }
    kx_die("unknown function");
    return mknum(0);
}

static Value *lvalue_ref(Expr *e, int *is_field, int *field_no) {
    *is_field = 0;
    if (e->kind == E_FIELD) {
        *is_field = 1;
        *field_no = (int) to_num(eval(e->a));
        return NULL;
    }
    return NULL;
}

static Value assign_to(Expr *lhs, Value v) {
    int is_field, field_no = 0;
    lvalue_ref(lhs, &is_field, &field_no);
    char buf[512];
    if (is_field) {
        set_field(field_no, to_str(v, buf, sizeof buf));
    } else {
        set_var(lhs->str, v);
    }
    return v;
}

static Value eval(Expr *e) {
    char buf[64], buf2[64];
    switch (e->kind) {
    case E_NUM:
        return mknum(e->num);
    case E_STR:
        return mkstr(e->str);
    case E_ERE:
        return mknum(regexec(e->re, get_field(0), 0, NULL, 0) == 0);
    case E_FIELD:
        return mkstrnum(get_field((int) to_num(eval(e->a))));
    case E_VAR:
        return get_var(e->str);
    case E_ASSIGN:
        return assign_to(e->a, eval(e->b));
    case E_OPASSIGN: {
        Value cur = eval(e->a);
        Value rhs = eval(e->b);
        double x = to_num(cur), y = to_num(rhs), r = 0;
        switch (e->op) {
        case T_ADDA:
            r = x + y;
            break;
        case T_SUBA:
            r = x - y;
            break;
        case T_MULA:
            r = x * y;
            break;
        case T_DIVA:
            r = x / y;
            break;
        case T_MODA:
            r = fmod(x, y);
            break;
        default:
            break;
        }
        return assign_to(e->a, mknum(r));
    }
    case E_NEG:
        return mknum(-to_num(eval(e->a)));
    case E_POS:
        return mknum(to_num(eval(e->a)));
    case E_NOT:
        return mknum(!truthy(eval(e->a)));
    case E_AND:
        return mknum(truthy(eval(e->a)) && truthy(eval(e->b)));
    case E_OR:
        return mknum(truthy(eval(e->a)) || truthy(eval(e->b)));
    case E_MATCH: {
        Value sv = eval(e->a);
        const char *s = to_str(sv, buf, sizeof buf);
        int m;
        if (e->b->kind == E_ERE) {
            m = regexec(e->b->re, s, 0, NULL, 0) == 0;
        } else {
            Value pv = eval(e->b);
            regex_t re;
            const char *pat = to_str(pv, buf2, sizeof buf2);
            if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) kx_die("bad regex");
            m = regexec(&re, s, 0, NULL, 0) == 0;
            regfree(&re);
        }
        return mknum(e->op ? !m : m);
    }
    case E_CONCAT: {
        char b1[64], b2[64];
        Value a = eval(e->a), b = eval(e->b);
        const char *sa = to_str(a, b1, sizeof b1), *sb = to_str(b, b2, sizeof b2);
        char *r = malloc(strlen(sa) + strlen(sb) + 1);
        strcpy(r, sa);
        strcat(r, sb);
        Value ret = mkstr(r);
        free(r);
        return ret;
    }
    case E_TERNARY:
        return truthy(eval(e->a)) ? eval(e->b) : eval(e->c);
    case E_PREINC:
        return assign_to(e->a, mknum(to_num(eval(e->a)) + 1));
    case E_PREDEC:
        return assign_to(e->a, mknum(to_num(eval(e->a)) - 1));
    case E_POSTINC: {
        Value old = eval(e->a);
        assign_to(e->a, mknum(to_num(old) + 1));
        return mknum(to_num(old));
    }
    case E_POSTDEC: {
        Value old = eval(e->a);
        assign_to(e->a, mknum(to_num(old) - 1));
        return mknum(to_num(old));
    }
    case E_CALL:
        return call_builtin(e->str, e->args);
    case E_BINOP: {
        if (e->op == '+' || e->op == '-' || e->op == '*' || e->op == '/' || e->op == '%' ||
            e->op == '^') {
            double x = to_num(eval(e->a)), y = to_num(eval(e->b));
            switch (e->op) {
            case '+':
                return mknum(x + y);
            case '-':
                return mknum(x - y);
            case '*':
                return mknum(x * y);
            case '/':
                return mknum(x / y);
            case '%':
                return mknum(fmod(x, y));
            case '^':
                return mknum(pow(x, y));
            }
        }
        Value a = eval(e->a), b = eval(e->b);
        int c = compare(a, b);
        switch (e->op) {
        case T_LT:
            return mknum(c < 0);
        case T_LE:
            return mknum(c <= 0);
        case T_GT:
            return mknum(c > 0);
        case T_GE:
            return mknum(c >= 0);
        case T_EQ:
            return mknum(c == 0);
        case T_NE:
            return mknum(c != 0);
        }
        return mknum(0);
    }
    }
    return mknum(0);
}

static ExecSig exec_stmt(Stmt *s);

static ExecSig exec_list(Stmt *s) {
    for (; s; s = s->next) {
        ExecSig sig = exec_stmt(s);
        if (sig != EX_NORMAL) return sig;
    }
    return EX_NORMAL;
}

static void print_args(FILE *out, ExprList *args) {
    if (!args) {
        fputs(get_field(0), out);
        fputs(ors_val, out);
        return;
    }
    char buf[512];
    for (ExprList *a = args; a; a = a->next) {
        Value v = eval(a->e);
        fputs(to_str(v, buf, sizeof buf), out);
        if (a->next) fputs(ofs_val, out);
    }
    fputs(ors_val, out);
}

static ExecSig exec_stmt(Stmt *s) {
    switch (s->kind) {
    case S_BLOCK:
        return exec_list(s->body);
    case S_EXPR:
        eval(s->cond);
        return EX_NORMAL;
    case S_PRINT:
        print_args(stdout, s->args);
        return EX_NORMAL;
    case S_PRINTF: {
        char buf[512];
        Value fv = eval(s->args->e);
        do_printf(stdout, to_str(fv, buf, sizeof buf), s->args->next);
        return EX_NORMAL;
    }
    case S_IF:
        if (truthy(eval(s->cond))) return exec_stmt(s->body);
        if (s->els) return exec_stmt(s->els);
        return EX_NORMAL;
    case S_WHILE:
        while (truthy(eval(s->cond))) {
            ExecSig sig = exec_stmt(s->body);
            if (sig == EX_BREAK) break;
            if (sig == EX_NEXT || sig == EX_EXIT) return sig;
        }
        return EX_NORMAL;
    case S_FOR:
        if (s->init) exec_stmt(s->init);
        while (!s->cond || truthy(eval(s->cond))) {
            ExecSig sig = exec_stmt(s->body);
            if (sig == EX_BREAK) break;
            if (sig == EX_NEXT || sig == EX_EXIT) return sig;
            if (s->post) exec_stmt(s->post);
        }
        return EX_NORMAL;
    case S_NEXT:
        return EX_NEXT;
    case S_BREAK:
        return EX_BREAK;
    case S_CONTINUE:
        return EX_CONTINUE;
    case S_EXIT:
        exit_code = s->cond ? (int) to_num(eval(s->cond)) : 0;
        return EX_EXIT;
    }
    return EX_NORMAL;
}

static ExecSig run_items(Item *items, PatKind want) {
    for (Item *it = items; it; it = it->next) {
        if (it->kind != want) continue;
        if (want == PAT_EXPR && it->expr && !truthy(eval(it->expr))) continue;
        ExecSig sig = it->action ? exec_stmt(it->action) : (print_args(stdout, NULL), EX_NORMAL);
        if (sig == EX_NEXT) return EX_NORMAL;
        if (sig == EX_EXIT) return EX_EXIT;
    }
    return EX_NORMAL;
}

static void process_file(FILE *f, Item *items, int *exited) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while (!*exited && (n = getline(&line, &cap, f)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = 0;
        nr_val++;
        set_record(line);
        if (run_items(items, PAT_ALWAYS) == EX_EXIT) {
            *exited = 1;
            break;
        }
        if (run_items(items, PAT_EXPR) == EX_EXIT) {
            *exited = 1;
            break;
        }
    }
    free(line);
}

int main(int argc, char **argv) {
    kx_prog = "awk";
    int i = 1;
    ExprList dummy = { 0 };
    (void) dummy;
    char *progtext = NULL;
    ExprList *pending_v = NULL, **pending_v_tail = &pending_v;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            snprintf(fs, sizeof fs, "%s", argv[++i]);
            unescape_inplace(fs);
            i++;
        } else if (strncmp(argv[i], "-F", 2) == 0) {
            snprintf(fs, sizeof fs, "%s", argv[i] + 2);
            unescape_inplace(fs);
            i++;
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            char *assign = argv[++i];
            char *eq = strchr(assign, '=');
            if (eq) {
                *eq = 0;
                ExprList *node = calloc(1, sizeof(ExprList));
                Expr *ve = new_expr(E_STR);
                ve->str = strdup(assign);
                node->e = ve;
                Expr *ve2 = new_expr(E_STR);
                ve2->str = strdup(eq + 1);
                node->next = NULL;
                ExprList *valnode = calloc(1, sizeof(ExprList));
                valnode->e = ve2;
                node->next = valnode;
                *pending_v_tail = node;
                pending_v_tail = &node->next->next;
            }
            i++;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            break;
        }
    }
    if (i >= argc) kx_die("usage: awk [-F fs] [-v var=value] 'program' [file...]");
    progtext = argv[i++];

    for (ExprList *p = pending_v; p; p = p->next->next) {
        set_var(p->e->str, mkstr(p->next->e->str));
    }

    Item *items = parse_program(progtext);

    int have_main = 0, have_end = 0;
    for (Item *it = items; it; it = it->next) {
        if (it->kind == PAT_ALWAYS || it->kind == PAT_EXPR) have_main = 1;
        if (it->kind == PAT_END) have_end = 1;
    }

    set_var("FS", mkstr(fs));
    int exited = 0;
    int rc = 0;
    ExecSig sig = run_items(items, PAT_BEGIN);
    if (sig == EX_EXIT) exited = 1;

    if (!exited && (have_main || have_end)) {
        if (i == argc) {
            process_file(stdin, items, &exited);
        } else {
            for (; i < argc && !exited; i++) {
                FILE *f = fopen(argv[i], "r");
                if (!f) {
                    kx_warn(argv[i]);
                    rc = 2;
                    continue;
                }
                filename_val = argv[i];
                process_file(f, items, &exited);
                fclose(f);
            }
        }
    }

    run_items(items, PAT_END);
    return exit_code ? exit_code : rc;
}
