#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#include <sys/timeb.h>
#include <windows.h>
#else
#include <strings.h>
#include <sys/time.h>
#endif
#include <time.h>

typedef enum {
    TOKEN_EOF, TOKEN_NEWLINE, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_COMMA, TOKEN_COLON, TOKEN_DOT, TOKEN_PLUS, TOKEN_MINUS,
    TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT, TOKEN_EQEQ, TOKEN_EQ, TOKEN_BANGEQ, TOKEN_LESSEQ, TOKEN_LESS,
    TOKEN_GREATEREQ, TOKEN_GREATER, TOKEN_STRING, TOKEN_DECIMAL, TOKEN_NUMBER, TOKEN_IDENTIFIER,
    TOKEN_SAY, TOKEN_VAR, TOKEN_TEXT, TOKEN_NUMBER_KW, TOKEN_DECIMAL_KW, TOKEN_BOOLEAN_KW,
    TOKEN_GET, TOKEN_ARRAY, TOKEN_DICTIONARY, TOKEN_JOB, TOKEN_IF, TOKEN_ELSE, TOKEN_REPEAT,
    TOKEN_FOREVER, TOKEN_OUT, TOKEN_FILE, TOKEN_CREATE, TOKEN_UPDATE, TOKEN_DELETE, TOKEN_SET,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_IMPORT, TOKEN_AS, TOKEN_TIME, TOKEN_SLEEP
} EasecTokenType;

typedef struct {
    EasecTokenType type;
    char* text;
    int line;
    int col;
} EasecToken;

typedef enum {
    EXPR_LITERAL, EXPR_VAR, EXPR_BINOP, EXPR_UNARY, EXPR_CALL, EXPR_MEMBER,
    EXPR_ARRAY_GET, EXPR_DICT_GET, EXPR_TIME_GET, EXPR_TIME_SLEEP
} ExprType;

typedef enum {
    STMT_EXPR, STMT_SAY, STMT_VAR, STMT_ARRAY, STMT_ARRAY_SET, STMT_DICT,
    STMT_DICT_SET, STMT_JOB, STMT_IF, STMT_REPEAT, STMT_OUT, STMT_FILE,
    STMT_IMPORT, STMT_ASSIGN
} StmtType;

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

int allow_implicit_call = 1;

long long get_time_ms() {
#ifdef _MSC_VER
    struct __timeb64 timebuffer;
    _ftime64(&timebuffer);
    return (long long)(timebuffer.time) * 1000 + timebuffer.millitm;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
#endif
}

void sleep_ms(long long ms) {
#ifdef _MSC_VER
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

size_t bytes_allocated = 0;

void* safe_alloc(size_t size) {
    size_t* ptr = (size_t*)malloc(size + sizeof(size_t));
    if (!ptr) { fprintf(stderr, "Fatal: Out of memory.\n"); exit(1); }
    memset(ptr + 1, 0, size);
    *ptr = size;
    bytes_allocated += size;
    return (void*)(ptr + 1);
}

void safe_free(void* p) {
    if (!p) return;
    size_t* ptr = (size_t*)p - 1;
    bytes_allocated -= *ptr;
    free(ptr);
}

void* safe_realloc(void* p, size_t new_size) {
    if (!p) return safe_alloc(new_size);
    if (new_size == 0) { safe_free(p); return NULL; }
    size_t* ptr = (size_t*)p - 1;
    size_t old_size = *ptr;
    size_t* new_ptr = (size_t*)realloc(ptr, new_size + sizeof(size_t));
    if (!new_ptr) { fprintf(stderr, "Fatal: Out of memory.\n"); exit(1); }
    *new_ptr = new_size;
    bytes_allocated -= old_size;
    bytes_allocated += new_size;
    return (void*)(new_ptr + 1);
}

char* safe_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)safe_alloc(len);
    memcpy(copy, s, len);
    return copy;
}

typedef struct ArenaBlock {
    char data[65536];
    int offset;
    struct ArenaBlock* next;
} ArenaBlock;
ArenaBlock* arena = NULL;

void* ast_alloc(size_t size) {
    size = (size + 7) & ~7;
    if (!arena || arena->offset + size > 65536) {
        ArenaBlock* block = (ArenaBlock*)malloc(sizeof(ArenaBlock));
        block->offset = 0;
        block->next = arena;
        arena = block;
    }
    void* ptr = arena->data + arena->offset;
    arena->offset += size;
    return ptr;
}

char* ast_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)ast_alloc(len);
    memcpy(copy, s, len);
    return copy;
}

void* ast_realloc(void* ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) return NULL;
    void* new_ptr = ast_alloc(new_size);
    if (ptr && old_size > 0) memcpy(new_ptr, ptr, old_size);
    return new_ptr;
}

#define AST_REALLOC_ARRAY(ptr, type, old_count, new_count) \
    (type*)ast_realloc(ptr, sizeof(type) * (old_count), sizeof(type) * (new_count))

void free_ast() {
    ArenaBlock* curr = arena;
    while (curr) {
        ArenaBlock* next = curr->next;
        free(curr);
        curr = next;
    }
    arena = NULL;
}

typedef enum { VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_OBJ } ValType;
typedef enum { OBJ_STRING, OBJ_ARRAY, OBJ_DICT, OBJ_JOB, OBJ_MODULE, OBJ_ENV } ObjType;

typedef struct sObject {
    ObjType type;
    int marked;
    int is_constant;
    struct sObject* next;
} Object;

typedef struct {
    ValType type;
    union {
        int boolean;
        long long integer;
        double floating;
        Object* obj;
    } as;
} Value;

typedef struct {
    struct sObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

typedef struct sEnv {
    Object obj;
    Table variables;
    struct sEnv* parent;
} Env;

typedef struct sObjString {
    Object obj;
    char* chars;
    uint32_t hash;
} ObjString;

typedef struct { Object obj; Value* items; int capacity; int count; } ObjArray;
typedef struct { Object obj; Table table; } ObjDict;

Value make_null(void);
Value make_bool(int b);
Value make_int(long long i);
Value make_float(double f);
void mark_value(Value val);
void run_file(const char* path, Env* env);

typedef enum {
    OP_CONSTANT,
    OP_NULL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_DUP,
    OP_GET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_PROPERTY,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_NOT,
    OP_NEGATE,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_RETURN,
    OP_CLOSURE,
    OP_ARRAY,
    OP_ARRAY_GET,
    OP_ARRAY_SET,
    OP_DICT,
    OP_DICT_GET,
    OP_DICT_SET,
    OP_GET_INPUT,
    OP_SAY,
    OP_FILE,
    OP_TIME_GET,
    OP_TIME_SLEEP,
    OP_IMPORT,
} OpCode;

typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    int* lines;
    Value* constants;
    int constant_count;
    int constant_capacity;
} Chunk;

typedef struct {
    Object obj;
    ObjString* name;
    int arity;
    ObjString** params;
    Chunk chunk;
    Env* closure;
} ObjJob;

typedef struct { Object obj; Env* env; } ObjModule;

void init_table(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void free_table(Table* table) {
    safe_free(table->entries);
    init_table(table);
}

static Entry* find_entry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % capacity;
    Entry* tombstone = NULL;
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == NULL) {
            if (entry->value.type == VAL_NULL) {
                return tombstone != NULL ? tombstone : entry;
            } else {
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) % capacity;
    }
}

int table_get(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return 0;
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;
    *value = entry->value;
    return 1;
}

static void adjust_capacity(Table* table, int capacity) {
    Entry* entries = (Entry*)safe_alloc(sizeof(Entry) * capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value.type = VAL_NULL;
    }
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;
        Entry* dest = find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }
    safe_free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
}

int table_set(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * 0.75) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjust_capacity(table, capacity);
    }
    Entry* entry = find_entry(table->entries, table->capacity, key);
    int is_new_key = entry->key == NULL;
    if (is_new_key && entry->value.type == VAL_NULL) table->count++;
    entry->key = key;
    entry->value = value;
    return is_new_key;
}

int table_delete(Table* table, ObjString* key) {
    if (table->count == 0) return 0;
    Entry* entry = find_entry(table->entries, table->capacity, key);
    if (entry->key == NULL) return 0;
    entry->key = NULL;
    entry->value.type = VAL_BOOL;
    entry->value.as.boolean = 1;
    return 1;
}

ObjString* table_find_string(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t index = hash % table->capacity;
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            if (entry->value.type == VAL_NULL) return NULL;
        } else if (strlen(entry->key->chars) == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) % table->capacity;
    }
}

#define STACK_MAX 1024
#define FRAMES_MAX 64

typedef struct {
    ObjJob* job;
    uint8_t* ip;
    Value* slots;
    Env* env;
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Value stack[STACK_MAX];
    Value* stack_top;
    Table strings;
    Object* objects;
    size_t next_gc;
    int gc_paused;
    Env* env;
    Env** env_stack;
    int env_count;
    int env_capacity;
    
        char** import_stack;
    int import_count;
    int import_capacity;
} VM;

VM vm;

void init_vm() {
    vm.frame_count = 0;
    vm.stack_top = vm.stack;
    vm.objects = NULL;
    vm.next_gc = 1024 * 1024;
    vm.gc_paused = 0;
    init_table(&vm.strings);
    vm.env = NULL;
    vm.env_capacity = 64;
    vm.env_stack = (Env**)safe_alloc(sizeof(Env*) * vm.env_capacity);
    vm.env_count = 0;
    vm.import_stack = NULL;
    vm.import_count = 0;
    vm.import_capacity = 0;
}

#define OBJ_VAL(o) ((Value){VAL_OBJ, {.obj = (Object*)(o)}})

void push(Value value) {
    *vm.stack_top = value;
    vm.stack_top++;
}

Value pop() {
    vm.stack_top--;
    return *vm.stack_top;
}

Value peek(int distance) {
    return vm.stack_top[-1 - distance];
}

int had_runtime_error = 0;
void runtime_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
    had_runtime_error = 1;
}

uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

Object* allocate_object(size_t size, ObjType type);

ObjString* allocate_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    ObjString* interned = table_find_string(&vm.strings, chars, length, hash);
    if (interned != NULL) return interned;

    ObjString* string = (ObjString*)allocate_object(sizeof(ObjString), OBJ_STRING);
    string->chars = (char*)safe_alloc(length + 1);
    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    string->hash = hash;

    push(OBJ_VAL(string));
    table_set(&vm.strings, string, make_null());
    pop();

    return string;
}

Env* create_env(Env* parent) {
    Env* env = (Env*)allocate_object(sizeof(Env), OBJ_ENV);
    init_table(&env->variables);
    env->parent = parent;
    if (vm.env_count >= vm.env_capacity) {
        vm.env_capacity *= 2;
        vm.env_stack = (Env**)safe_realloc(vm.env_stack, sizeof(Env*) * vm.env_capacity);
    }
    vm.env_stack[vm.env_count++] = env;
    return env;
}

void pop_env() { if (vm.env_count > 0) vm.env_count--; }

void env_define(Env* env, const char* name, Value val) {
    ObjString* key = allocate_string(name, strlen(name));
    table_set(&env->variables, key, val);
}

int env_set(Env* env, const char* name, Value val) {
    ObjString* key = allocate_string(name, strlen(name));
    Env* curr = env;
    while (curr) {
        Value dummy;
        if (table_get(&curr->variables, key, &dummy)) {
            table_set(&curr->variables, key, val);
            return 1;
        }
        curr = curr->parent;
    }
    return 0;
}

Value env_get(Env* env, const char* name) {
    ObjString* key = allocate_string(name, strlen(name));
    Env* curr = env;
    while (curr) {
        Value val;
        if (table_get(&curr->variables, key, &val)) {
            return val;
        }
        curr = curr->parent;
    }
    return make_null();
}

void mark_object(Object* obj);

void mark_table(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            mark_object((Object*)entry->key);
            mark_value(entry->value);
        }
    }
}

void mark_env(Env* env) {
    if (!env || env->obj.marked) return;
    env->obj.marked = 1;
    mark_table(&env->variables);
    if (env->parent) mark_env(env->parent);
}

void init_chunk(Chunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->constants = NULL;
    chunk->constant_count = 0;
    chunk->constant_capacity = 0;
}

void free_chunk(Chunk* chunk) {
    safe_free(chunk->code);
    safe_free(chunk->lines);
    safe_free(chunk->constants);
    init_chunk(chunk);
}

void mark_object(Object* obj) {
    if (!obj || obj->marked) return;
    obj->marked = 1;
    if (obj->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)obj;
        for (int i = 0; i < arr->count; i++) mark_value(arr->items[i]);
    } else if (obj->type == OBJ_DICT) {
        ObjDict* dict = (ObjDict*)obj;
        mark_table(&dict->table);
    } else if (obj->type == OBJ_MODULE) {
        mark_env(((ObjModule*)obj)->env);
    } else if (obj->type == OBJ_JOB) {
        ObjJob* job = (ObjJob*)obj;
        mark_object((Object*)job->name);
        for (int i = 0; i < job->arity; i++) {
            mark_object((Object*)job->params[i]);
        }
        for (int i = 0; i < job->chunk.constant_count; i++) {
            mark_value(job->chunk.constants[i]);
        }
        mark_env(job->closure);
    } else if (obj->type == OBJ_ENV) {
        mark_env((Env*)obj);
    }
}

void mark_value(Value val) {
    if (val.type == VAL_OBJ) mark_object(val.as.obj);
}

void table_remove_white(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.marked) {
            table_delete(table, entry->key);
        }
    }
}

void gc_collect() {
    if (vm.gc_paused) return;
    
        for (Value* slot = vm.stack; slot < vm.stack_top; slot++) {
        mark_value(*slot);
    }
    
        for (int i = 0; i < vm.frame_count; i++) {
        mark_object((Object*)vm.frames[i].job);
        if (vm.frames[i].env != NULL) mark_env(vm.frames[i].env);
    }
    
    if (vm.env != NULL) mark_env(vm.env);
    
    table_remove_white(&vm.strings);
    
    Object** object = &vm.objects;
    while (*object != NULL) {
        if (!(*object)->marked && !(*object)->is_constant) {
            Object* unreached = *object;
            *object = unreached->next;
            
            if (unreached->type == OBJ_STRING) {
                safe_free(((ObjString*)unreached)->chars);
            } else if (unreached->type == OBJ_ARRAY) {
                safe_free(((ObjArray*)unreached)->items);
            } else if (unreached->type == OBJ_DICT) {
                ObjDict* dict = (ObjDict*)unreached;
                free_table(&dict->table);
            } else if (unreached->type == OBJ_ENV) {
                Env* env = (Env*)unreached;
                free_table(&env->variables);
            } else if (unreached->type == OBJ_JOB) {
                ObjJob* job = (ObjJob*)unreached;
                free_chunk(&job->chunk);
                safe_free(job->params);
            }
            
            safe_free(unreached);
        } else {
            (*object)->marked = 0;
            object = &(*object)->next;
        }
    }
    vm.next_gc = bytes_allocated * 2;
}

Object* allocate_object(size_t size, ObjType type) {
    if (bytes_allocated > vm.next_gc) gc_collect();
    Object* obj = (Object*)safe_alloc(size);
    obj->type = type;
    obj->marked = 0;
    obj->is_constant = 0;
    obj->next = vm.objects;
    vm.objects = obj;
    return obj;
}

Value make_array() {
    ObjArray* arr = (ObjArray*)allocate_object(sizeof(ObjArray), OBJ_ARRAY);
    arr->items = NULL; arr->capacity = 0; arr->count = 0;
    Value v; v.type = VAL_OBJ; v.as.obj = (Object*)arr;
    return v;
}

Value make_dict() {
    ObjDict* dict = (ObjDict*)allocate_object(sizeof(ObjDict), OBJ_DICT);
    init_table(&dict->table);
    Value v; v.type = VAL_OBJ; v.as.obj = (Object*)dict;
    return v;
}

Value make_null() { Value v; v.type = VAL_NULL; return v; }
Value make_bool(int b) { Value v; v.type = VAL_BOOL; v.as.boolean = b; return v; }
Value make_int(long long i) { Value v; v.type = VAL_INT; v.as.integer = i; return v; }
Value make_float(double f) { Value v; v.type = VAL_FLOAT; v.as.floating = f; return v; }

void write_chunk(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->count + 1 > chunk->capacity) {
        int capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code = safe_realloc(chunk->code, sizeof(uint8_t) * capacity);
        chunk->lines = safe_realloc(chunk->lines, sizeof(int) * capacity);
        chunk->capacity = capacity;
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int add_constant(Chunk* chunk, Value value) {
    push(value);
    if (chunk->constant_count + 1 > chunk->constant_capacity) {
        int capacity = chunk->constant_capacity < 8 ? 8 : chunk->constant_capacity * 2;
        chunk->constants = safe_realloc(chunk->constants, sizeof(Value) * capacity);
        chunk->constant_capacity = capacity;
    }
    chunk->constants[chunk->constant_count] = value;
    int index = chunk->constant_count++;
    pop();
    return index;
}

typedef struct { const char* source; int current; int line; int col; } Lexer;

Lexer lexer;
void init_lexer(const char* source) { lexer.source = source; lexer.current = 0; lexer.line = 1; lexer.col = 1; }

EasecToken make_token(EasecTokenType type, int start, int length) {
    EasecToken t;
    t.type = type;
    t.text = (char*)ast_alloc(length + 1);
    strncpy(t.text, lexer.source + start, length);
    t.text[length] = '\0';
    t.line = lexer.line; t.col = lexer.col - length;
    return t;
}

int is_alpha(char c) { return isalpha(c) || c == '_'; }
int is_digit(char c) { return isdigit(c); }
char lexer_advance() { lexer.col++; return lexer.source[lexer.current++]; }
char lexer_peek() { return lexer.source[lexer.current]; }
char lexer_peek_next() { return lexer.current + 1 < strlen(lexer.source) ? lexer.source[lexer.current + 1] : '\0'; }
int lexer_match(char expected) { if (lexer_peek() != expected) return 0; lexer_advance(); return 1; }

void skip_whitespace() {
    while (1) {
        char c = lexer_peek();
        if (c == ' ' || c == '\r' || c == '\t') { lexer_advance(); }
        else if (c == 'n' && lexer_peek_next() == 'o' && strncmp(lexer.source + lexer.current, "note", 4) == 0) {
            lexer.current += 4; lexer.col += 4;
            while (lexer_peek() == ' ' || lexer_peek() == '\t') lexer_advance();
            if (lexer_peek() == '[') {
                int depth = 1; lexer_advance();
                while (lexer_peek() != '\0' && depth > 0) {
                    if (lexer_peek() == '[') depth++;
                    else if (lexer_peek() == ']') depth--;
                    if (lexer_peek() == '\n') { lexer.line++; lexer.col = 0; }
                    lexer_advance();
                }
            } else {
                while (lexer_peek() != '\n' && lexer_peek() != '\0') lexer_advance();
            }
        } else break;
    }
}

EasecToken next_token() {
    skip_whitespace();
    int start = lexer.current;
    if (lexer_peek() == '\0') return make_token(TOKEN_EOF, start, 0);

    char c = lexer_advance();
    if (c == '\n') { lexer.line++; lexer.col = 1; return make_token(TOKEN_NEWLINE, start, 1); }
    if (c == '(') return make_token(TOKEN_LPAREN, start, 1); if (c == ')') return make_token(TOKEN_RPAREN, start, 1);
    if (c == '[') return make_token(TOKEN_LBRACKET, start, 1); if (c == ']') return make_token(TOKEN_RBRACKET, start, 1);
    if (c == '{') return make_token(TOKEN_LBRACE, start, 1); if (c == '}') return make_token(TOKEN_RBRACE, start, 1);
    if (c == ',') return make_token(TOKEN_COMMA, start, 1); if (c == ':') return make_token(TOKEN_COLON, start, 1);
    if (c == '.') return make_token(TOKEN_DOT, start, 1); if (c == '+') return make_token(TOKEN_PLUS, start, 1);
    if (c == '-') return make_token(TOKEN_MINUS, start, 1); if (c == '*') return make_token(TOKEN_STAR, start, 1);
    if (c == '/') return make_token(TOKEN_SLASH, start, 1);
    if (c == '%') return make_token(TOKEN_PERCENT, start, 1);
    if (c == '=') return lexer_match('=') ? make_token(TOKEN_EQEQ, start, 2) : make_token(TOKEN_EQ, start, 1);
    if (c == '!') return lexer_match('=') ? make_token(TOKEN_BANGEQ, start, 2) : make_token(TOKEN_EOF, start, 1);
    if (c == '<') return lexer_match('=') ? make_token(TOKEN_LESSEQ, start, 2) : make_token(TOKEN_LESS, start, 1);
    if (c == '>') return lexer_match('=') ? make_token(TOKEN_GREATEREQ, start, 2) : make_token(TOKEN_GREATER, start, 1);

    if (c == '"') {
        while (lexer_peek() != '"' && lexer_peek() != '\0') {
            if (lexer_peek() == '\n') { lexer.line++; lexer.col = 1; }
            lexer_advance();
        }
        if (lexer_peek() == '\0') return make_token(TOKEN_EOF, start, 0);
        lexer_advance();
        return make_token(TOKEN_STRING, start + 1, lexer.current - start - 2);
    }

    if (is_digit(c)) {
        int is_dec = 0;
        while (is_digit(lexer_peek())) lexer_advance();
        if (lexer_peek() == '.' && is_digit(lexer_peek_next())) {
            is_dec = 1; lexer_advance();
            while (is_digit(lexer_peek())) lexer_advance();
        }
        return make_token(is_dec ? TOKEN_DECIMAL : TOKEN_NUMBER, start, lexer.current - start);
    }

    if (is_alpha(c)) {
        while (is_alpha(lexer_peek()) || is_digit(lexer_peek())) lexer_advance();
        int len = lexer.current - start;
        char* text = (char*)ast_alloc(len + 1);
        strncpy(text, lexer.source + start, len);
        text[len] = '\0';
        
        EasecTokenType type = TOKEN_IDENTIFIER;
        if (strcmp(text, "say") == 0) type = TOKEN_SAY; else if (strcmp(text, "var") == 0) type = TOKEN_VAR;
        else if (strcmp(text, "text") == 0) type = TOKEN_TEXT; else if (strcmp(text, "number") == 0) type = TOKEN_NUMBER_KW;
        else if (strcmp(text, "decimal") == 0) type = TOKEN_DECIMAL_KW; else if (strcmp(text, "boolean") == 0) type = TOKEN_BOOLEAN_KW;
        else if (strcmp(text, "get") == 0) type = TOKEN_GET; else if (strcmp(text, "array") == 0) type = TOKEN_ARRAY;
        else if (strcmp(text, "dictionary") == 0) type = TOKEN_DICTIONARY; else if (strcmp(text, "job") == 0) type = TOKEN_JOB;
        else if (strcmp(text, "if") == 0) type = TOKEN_IF; else if (strcmp(text, "else") == 0) type = TOKEN_ELSE;
        else if (strcmp(text, "repeat") == 0) type = TOKEN_REPEAT; else if (strcmp(text, "forever") == 0) type = TOKEN_FOREVER;
        else if (strcmp(text, "out") == 0) type = TOKEN_OUT; else if (strcmp(text, "file") == 0) type = TOKEN_FILE;
        else if (strcmp(text, "create") == 0) type = TOKEN_CREATE; else if (strcmp(text, "update") == 0) type = TOKEN_UPDATE;
        else if (strcmp(text, "delete") == 0) type = TOKEN_DELETE; else if (strcmp(text, "set") == 0) type = TOKEN_SET;
        else if (strcmp(text, "true") == 0) type = TOKEN_TRUE; else if (strcmp(text, "false") == 0) type = TOKEN_FALSE;
        else if (strcmp(text, "import") == 0) type = TOKEN_IMPORT; else if (strcmp(text, "as") == 0) type = TOKEN_AS;
        else if (strcmp(text, "time") == 0) type = TOKEN_TIME; else if (strcmp(text, "sleep") == 0) type = TOKEN_SLEEP;
        return make_token(type, start, len);
    }
    return make_token(TOKEN_EOF, start, 0);
}

typedef struct sExpr {
    ExprType type; int line;
    union {
        Value literal; char* name;
        struct { struct sExpr* left; EasecTokenType op; struct sExpr* right; } bin;
        struct { EasecTokenType op; struct sExpr* right; } unary;
        struct { struct sExpr* callee; struct sExpr** args; int count; } call;
        struct { struct sExpr* object; char* prop; } member;
        struct { char* name; struct sExpr* index; } array_get;
        struct { char* name; char* key; } dict_get;
        struct { struct sExpr* ms; } time_sleep;
    } as;
} Expr;

typedef struct sStmt {
    StmtType type; int line;
    union {
        Expr* expr;
        struct { char* name; Expr* initializer; int is_get; } var_decl;
        struct { char* name; Expr** elements; int count; } arr_decl;
        struct { char* name; char** keys; Expr** values; int count; } dict_decl;
        struct { char* name; char** params; int param_count; struct sStmt** body; int body_count; } job_decl;
        struct { Expr* cond; struct sStmt** then_b; int then_c; struct sStmt** else_b; int else_c; } if_stmt;
        struct { int forever; Expr* count; struct sStmt** body; int body_count; } repeat_stmt;
        struct { char* action; Expr* file; Expr* content; } file_stmt;
        struct { char* path; char* alias; } import_stmt;
        struct { char* name; Expr* value; } assign_stmt;
        struct { char* name; Expr* index; Expr* value; } array_set;
        struct { char* name; char* key; Expr* value; } dict_set;
    } as;
} Stmt;

Expr* make_expr(ExprType type, int line) { Expr* e = (Expr*)ast_alloc(sizeof(Expr)); e->type = type; e->line = line; return e; }
Stmt* make_stmt(StmtType type, int line) { Stmt* s = (Stmt*)ast_alloc(sizeof(Stmt)); s->type = type; s->line = line; return s; }
Expr* make_error_expr(); Stmt* make_error_stmt();

EasecToken parser_curr, parser_prev;
int had_error = 0;

void advance_parser() {
    parser_prev = parser_curr;
    parser_curr = next_token();
}

void error_at(EasecToken* token, const char* message) {
    if (had_error) return;
    fprintf(stderr, "Parse Error line %d: %s. Got '%s'\n", token->line, message, token->text);
    had_error = 1;
}

int match_token(EasecTokenType type) {
    if (parser_curr.type == type) { advance_parser(); return 1; }
    return 0;
}

void consume(EasecTokenType type, const char* err) {
    if (parser_curr.type == type) { advance_parser(); return; }
    error_at(&parser_curr, err);
}

void skip_newlines() { while (parser_curr.type == TOKEN_NEWLINE) advance_parser(); }

EasecToken peek_next_token_parser() {
    Lexer saved = lexer; EasecToken curr = parser_curr; EasecToken prev = parser_prev;
    advance_parser(); EasecToken next = parser_curr;
    lexer = saved; parser_curr = curr; parser_prev = prev;
    return next;
}

void synchronize() {
    while (parser_curr.type != TOKEN_EOF) {
        if (parser_prev.type == TOKEN_NEWLINE) return;
        switch (parser_curr.type) {
            case TOKEN_VAR: case TOKEN_SAY: case TOKEN_IF: case TOKEN_REPEAT: case TOKEN_OUT:
            case TOKEN_ARRAY: case TOKEN_DICTIONARY: case TOKEN_JOB: case TOKEN_FILE: case TOKEN_IMPORT:
                return;
            default: break;
        }
        advance_parser();
    }
}

int is_expr_start(EasecTokenType type) {
    switch (type) {
        case TOKEN_IDENTIFIER: case TOKEN_NUMBER: case TOKEN_DECIMAL: case TOKEN_STRING:
        case TOKEN_TRUE: case TOKEN_FALSE: case TOKEN_MINUS: case TOKEN_LPAREN:
        case TOKEN_ARRAY: case TOKEN_DICTIONARY: case TOKEN_TIME: return 1;
        default: return 0;
    }
}

Expr* parse_expr(Precedence prec); Stmt* parse_statement();

Expr* make_error_expr() { Expr* e = make_expr(EXPR_LITERAL, parser_curr.line); e->as.literal = make_null(); return e; }
Stmt* make_error_stmt() { Stmt* s = make_stmt(STMT_EXPR, parser_curr.line); s->as.expr = make_error_expr(); return s; }

Expr* parse_literal() {
    Expr* e = make_expr(EXPR_LITERAL, parser_prev.line);
    if (parser_prev.type == TOKEN_NUMBER) e->as.literal = make_int(atoll(parser_prev.text));
    else if (parser_prev.type == TOKEN_DECIMAL) e->as.literal = make_float(atof(parser_prev.text));
    else if (parser_prev.type == TOKEN_STRING) { e->as.literal = OBJ_VAL(allocate_string(parser_prev.text, strlen(parser_prev.text))); e->as.literal.as.obj->is_constant = 1; }
    else if (parser_prev.type == TOKEN_TRUE) e->as.literal = make_bool(1);
    else if (parser_prev.type == TOKEN_FALSE) e->as.literal = make_bool(0);
    return e;
}

Expr* parse_variable() {
    Expr* e = make_expr(EXPR_VAR, parser_prev.line);
    e->as.name = ast_strdup(parser_prev.text);
    return e;
}

Expr* parse_grouping() { Expr* e = parse_expr(PREC_ASSIGN); consume(TOKEN_RPAREN, "Expected ')' after expression"); return e; }
Expr* parse_unary() {
    EasecTokenType op = parser_prev.type; Expr* e = make_expr(EXPR_UNARY, parser_prev.line);
    e->as.unary.op = op; e->as.unary.right = parse_expr(PREC_UNARY); return e;
}

Expr* parse_binary(Expr* left) {
    EasecTokenType op = parser_prev.type; Precedence prec = PREC_NONE;
    if (op == TOKEN_PLUS || op == TOKEN_MINUS) prec = PREC_TERM; else if (op == TOKEN_STAR || op == TOKEN_SLASH) prec = PREC_FACTOR;
    else if (op == TOKEN_PERCENT) prec = PREC_FACTOR;
    else if (op == TOKEN_EQEQ || op == TOKEN_BANGEQ) prec = PREC_EQUALITY;
    else if (op == TOKEN_LESS || op == TOKEN_LESSEQ || op == TOKEN_GREATER || op == TOKEN_GREATEREQ) prec = PREC_COMPARISON;
    Expr* right = parse_expr((Precedence)(prec + 1)); Expr* e = make_expr(EXPR_BINOP, parser_prev.line);
    e->as.bin.left = left; e->as.bin.op = op; e->as.bin.right = right; return e;
}

Expr* parse_dot(Expr* left) {
    consume(TOKEN_IDENTIFIER, "Expected property name"); Expr* e = make_expr(EXPR_MEMBER, parser_prev.line);
    e->as.member.object = left; e->as.member.prop = ast_strdup(parser_prev.text); return e;
}

Expr* parse_array_get() {
    int line = parser_prev.line; consume(TOKEN_GET, "Expected 'get' after array");
    if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected array name"); return make_error_expr(); }
    char* name = ast_strdup(parser_curr.text); advance_parser(); Expr* index = parse_expr(PREC_ASSIGN);
    Expr* e = make_expr(EXPR_ARRAY_GET, line); e->as.array_get.name = name; e->as.array_get.index = index; return e;
}

Expr* parse_dict_get() {
    int line = parser_prev.line; consume(TOKEN_GET, "Expected 'get' after dictionary");
    if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected dictionary name"); return make_error_expr(); }
    char* name = ast_strdup(parser_curr.text); advance_parser();
    if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected key name"); return make_error_expr(); }
    char* key = ast_strdup(parser_curr.text); advance_parser();
    Expr* e = make_expr(EXPR_DICT_GET, line); e->as.dict_get.name = name; e->as.dict_get.key = key; return e;
}

Expr* parse_time() {
    int line = parser_prev.line; 
    if (match_token(TOKEN_GET)) {
        return make_expr(EXPR_TIME_GET, line);
    } else if (match_token(TOKEN_SLEEP)) {
        Expr* e = make_expr(EXPR_TIME_SLEEP, line);
        e->as.time_sleep.ms = parse_expr(PREC_ASSIGN);
        return e;
    } else {
        error_at(&parser_curr, "Expected 'get' or 'sleep' after time");
        return make_error_expr();
    }
}

typedef struct { Expr* (*prefix)(); Expr* (*infix)(Expr*); Precedence prec; } ParseRule;
ParseRule* get_rule(EasecTokenType type);

Expr* parse_expr(Precedence prec) {
    advance_parser();
    ParseRule* prefixRule = get_rule(parser_prev.type);
    if (prefixRule->prefix == NULL) { error_at(&parser_prev, "Expected expression"); return make_error_expr(); }
    Expr* left = prefixRule->prefix();
    
    while (1) {
        ParseRule* infixRule = get_rule(parser_curr.type);
        if (allow_implicit_call && prec <= PREC_CALL && infixRule->infix == NULL && is_expr_start(parser_curr.type)) {
            Expr* call = make_expr(EXPR_CALL, parser_prev.line);
            call->as.call.callee = left;
            call->as.call.args = NULL;
            call->as.call.count = 0;
            do {
                if (parser_curr.type == TOKEN_NEWLINE || parser_curr.type == TOKEN_EOF || parser_curr.type == TOKEN_RBRACKET) break;
                call->as.call.args = AST_REALLOC_ARRAY(call->as.call.args, Expr*, call->as.call.count, call->as.call.count + 1);
                call->as.call.args[call->as.call.count++] = parse_expr(PREC_ASSIGN);
            } while (match_token(TOKEN_COMMA));
            left = call;
            continue;
        }
        if (prec <= infixRule->prec && infixRule->prec != PREC_NONE) {
            advance_parser();
            left = infixRule->infix(left);
            continue;
        }
        break;
    }
    return left;
}

ParseRule rules[] = {
    [TOKEN_LPAREN]    = {parse_grouping, NULL, PREC_CALL}, [TOKEN_DOT]       = {NULL, parse_dot, PREC_CALL},
    [TOKEN_MINUS]     = {parse_unary, parse_binary, PREC_TERM}, [TOKEN_PLUS]      = {NULL, parse_binary, PREC_TERM},
    [TOKEN_SLASH]     = {NULL, parse_binary, PREC_FACTOR}, [TOKEN_STAR]      = {NULL, parse_binary, PREC_FACTOR},
    [TOKEN_PERCENT]   = {NULL, parse_binary, PREC_FACTOR},
    [TOKEN_EQEQ]      = {NULL, parse_binary, PREC_EQUALITY}, [TOKEN_BANGEQ]    = {NULL, parse_binary, PREC_EQUALITY},
    [TOKEN_LESS]      = {NULL, parse_binary, PREC_COMPARISON}, [TOKEN_LESSEQ]    = {NULL, parse_binary, PREC_COMPARISON},
    [TOKEN_GREATER]   = {NULL, parse_binary, PREC_COMPARISON}, [TOKEN_GREATEREQ] = {NULL, parse_binary, PREC_COMPARISON},
    [TOKEN_NUMBER]    = {parse_literal, NULL, PREC_NONE}, [TOKEN_DECIMAL]   = {parse_literal, NULL, PREC_NONE},
    [TOKEN_STRING]    = {parse_literal, NULL, PREC_NONE}, [TOKEN_TRUE]      = {parse_literal, NULL, PREC_NONE},
    [TOKEN_FALSE]     = {parse_literal, NULL, PREC_NONE}, [TOKEN_IDENTIFIER]= {parse_variable, NULL, PREC_NONE},
    [TOKEN_ARRAY]     = {parse_array_get, NULL, PREC_NONE}, [TOKEN_DICTIONARY]= {parse_dict_get, NULL, PREC_NONE},
    [TOKEN_TIME]      = {parse_time, NULL, PREC_NONE},
};
ParseRule* get_rule(EasecTokenType type) {
    if (type >= sizeof(rules) / sizeof(rules[0])) { static ParseRule empty = {NULL, NULL, PREC_NONE}; return &empty; }
    return &rules[type];
}

Stmt** parse_block(int* count) {
    consume(TOKEN_LBRACKET, "Expected '[' to begin block"); skip_newlines();
    Stmt** stmts = NULL; *count = 0;
    while (parser_curr.type != TOKEN_RBRACKET && parser_curr.type != TOKEN_EOF) {
        stmts = AST_REALLOC_ARRAY(stmts, Stmt*, *count, *count + 1);
        stmts[(*count)++] = parse_statement();
        if (had_error) synchronize();
        skip_newlines();
    }
    consume(TOKEN_RBRACKET, "Expected ']' to end block"); return stmts;
}

Stmt* parse_statement() {
    int line = parser_curr.line;
    if (match_token(TOKEN_SAY)) { Stmt* s = make_stmt(STMT_SAY, line); s->as.expr = parse_expr(PREC_ASSIGN); return s; }
    if (match_token(TOKEN_VAR)) {
        if (parser_curr.type == TOKEN_TEXT || parser_curr.type == TOKEN_NUMBER_KW || parser_curr.type == TOKEN_DECIMAL_KW || parser_curr.type == TOKEN_BOOLEAN_KW) advance_parser();
        if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected variable name"); return make_error_stmt(); }
        char* name = ast_strdup(parser_curr.text); advance_parser();
        if (match_token(TOKEN_EQ)) {
            error_at(&parser_prev, "Variable declarations do not use '='. Use 'var <optional type> <name> <value>'");
            return make_error_stmt();
        }
        Stmt* s = make_stmt(STMT_VAR, line); s->as.var_decl.name = name;
        if (match_token(TOKEN_GET)) { s->as.var_decl.is_get = 1; s->as.var_decl.initializer = NULL; }
        else { s->as.var_decl.is_get = 0; s->as.var_decl.initializer = parse_expr(PREC_ASSIGN); }
        return s;
    }
    if (parser_curr.type == TOKEN_ARRAY) {
        EasecToken next = peek_next_token_parser();
        if (next.type == TOKEN_SET) {
            advance_parser(); advance_parser();
            if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected array name"); return make_error_stmt(); }
            char* name = ast_strdup(parser_curr.text); advance_parser();
            
            allow_implicit_call = 0;
            Expr* index = parse_expr(PREC_ASSIGN); 
            allow_implicit_call = 1;
            
            Expr* value = parse_expr(PREC_ASSIGN);
            Stmt* s = make_stmt(STMT_ARRAY_SET, line); s->as.array_set.name = name; s->as.array_set.index = index; s->as.array_set.value = value;
            return s;
        } else if (next.type != TOKEN_GET) {
            advance_parser();
            if (parser_curr.type == TOKEN_TEXT || parser_curr.type == TOKEN_NUMBER_KW || parser_curr.type == TOKEN_DECIMAL_KW || parser_curr.type == TOKEN_BOOLEAN_KW) advance_parser();
            if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected array name"); return make_error_stmt(); }
            Stmt* s = make_stmt(STMT_ARRAY, line); s->as.arr_decl.name = ast_strdup(parser_curr.text); advance_parser();
            s->as.arr_decl.elements = NULL; s->as.arr_decl.count = 0; skip_newlines();
            if (parser_curr.type != TOKEN_EOF && parser_curr.type != TOKEN_RBRACKET && parser_curr.type != TOKEN_NEWLINE) {
                do {
                    skip_newlines();
                    if (parser_curr.type == TOKEN_NEWLINE || parser_curr.type == TOKEN_EOF || parser_curr.type == TOKEN_RBRACKET) break;
                    s->as.arr_decl.elements = AST_REALLOC_ARRAY(s->as.arr_decl.elements, Expr*, s->as.arr_decl.count, s->as.arr_decl.count + 1);
                    s->as.arr_decl.elements[s->as.arr_decl.count++] = parse_expr(PREC_ASSIGN);
                    skip_newlines();
                } while (match_token(TOKEN_COMMA));
            }
            return s;
        }
    }
    if (parser_curr.type == TOKEN_DICTIONARY) {
        EasecToken next = peek_next_token_parser();
        if (next.type == TOKEN_SET) {
            advance_parser(); advance_parser();
            if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected dict name"); return make_error_stmt(); }
            char* name = ast_strdup(parser_curr.text); advance_parser();
            if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected key name"); return make_error_stmt(); }
            char* key = ast_strdup(parser_curr.text); advance_parser();
            Expr* value = parse_expr(PREC_ASSIGN);
            Stmt* s = make_stmt(STMT_DICT_SET, line); s->as.dict_set.name = name; s->as.dict_set.key = key; s->as.dict_set.value = value;
            return s;
        } else if (next.type != TOKEN_GET) {
            advance_parser();
            if (parser_curr.type == TOKEN_TEXT || parser_curr.type == TOKEN_NUMBER_KW || parser_curr.type == TOKEN_DECIMAL_KW || parser_curr.type == TOKEN_BOOLEAN_KW) advance_parser();
            if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected dict name"); return make_error_stmt(); }
            Stmt* s = make_stmt(STMT_DICT, line); s->as.dict_decl.name = ast_strdup(parser_curr.text); advance_parser();
            s->as.dict_decl.keys = NULL; s->as.dict_decl.values = NULL; s->as.dict_decl.count = 0; skip_newlines();
            if (parser_curr.type != TOKEN_EOF && parser_curr.type != TOKEN_RBRACKET && parser_curr.type != TOKEN_NEWLINE) {
                do {
                    skip_newlines();
                    if (parser_curr.type == TOKEN_NEWLINE || parser_curr.type == TOKEN_EOF || parser_curr.type == TOKEN_RBRACKET) break;
                    if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected key name"); break; }
                    char* key_name = ast_strdup(parser_curr.text); advance_parser(); consume(TOKEN_COLON, "Expected ':'");
                    s->as.dict_decl.keys = AST_REALLOC_ARRAY(s->as.dict_decl.keys, char*, s->as.dict_decl.count, s->as.dict_decl.count + 1);
                    s->as.dict_decl.values = AST_REALLOC_ARRAY(s->as.dict_decl.values, Expr*, s->as.dict_decl.count, s->as.dict_decl.count + 1);
                    s->as.dict_decl.keys[s->as.dict_decl.count] = key_name; s->as.dict_decl.values[s->as.dict_decl.count] = parse_expr(PREC_ASSIGN);
                    s->as.dict_decl.count++; skip_newlines();
                } while (match_token(TOKEN_COMMA));
            }
            return s;
        }
    }
    if (match_token(TOKEN_JOB)) {
        if (parser_curr.type != TOKEN_IDENTIFIER) { error_at(&parser_curr, "Expected job name"); return make_error_stmt(); }
        Stmt* s = make_stmt(STMT_JOB, line); s->as.job_decl.name = ast_strdup(parser_curr.text); advance_parser();
        s->as.job_decl.params = NULL; s->as.job_decl.param_count = 0; skip_newlines();
        
                if (parser_curr.type != TOKEN_LBRACKET) {
            while (parser_curr.type == TOKEN_IDENTIFIER) {
                s->as.job_decl.params = AST_REALLOC_ARRAY(s->as.job_decl.params, char*, s->as.job_decl.param_count, s->as.job_decl.param_count + 1);
                s->as.job_decl.params[s->as.job_decl.param_count++] = ast_strdup(parser_curr.text); advance_parser();
                skip_newlines(); if (!match_token(TOKEN_COMMA)) break; skip_newlines();
            }
        }
        skip_newlines(); s->as.job_decl.body = parse_block(&s->as.job_decl.body_count); return s;
    }
    if (match_token(TOKEN_IF)) {
        Stmt* s = make_stmt(STMT_IF, line); s->as.if_stmt.cond = parse_expr(PREC_ASSIGN); skip_newlines();
        s->as.if_stmt.then_b = parse_block(&s->as.if_stmt.then_c); s->as.if_stmt.else_b = NULL; s->as.if_stmt.else_c = 0; skip_newlines();
        if (match_token(TOKEN_ELSE)) { skip_newlines(); s->as.if_stmt.else_b = parse_block(&s->as.if_stmt.else_c); } return s;
    }
    if (match_token(TOKEN_REPEAT)) {
        Stmt* s = make_stmt(STMT_REPEAT, line);
        if (match_token(TOKEN_FOREVER)) s->as.repeat_stmt.forever = 1; else { s->as.repeat_stmt.forever = 0; s->as.repeat_stmt.count = parse_expr(PREC_ASSIGN); }
        skip_newlines(); s->as.repeat_stmt.body = parse_block(&s->as.repeat_stmt.body_count); return s;
    }
    if (match_token(TOKEN_OUT)) {
        Stmt* s = make_stmt(STMT_OUT, line); s->as.expr = (parser_curr.type != TOKEN_NEWLINE && parser_curr.type != TOKEN_RBRACKET && parser_curr.type != TOKEN_EOF) ? parse_expr(PREC_ASSIGN) : NULL; return s;
    }
    if (match_token(TOKEN_FILE)) {
        Stmt* s = make_stmt(STMT_FILE, line);
        if (match_token(TOKEN_CREATE)) s->as.file_stmt.action = "create"; else if (match_token(TOKEN_UPDATE)) s->as.file_stmt.action = "update";
        else if (match_token(TOKEN_DELETE)) s->as.file_stmt.action = "delete"; else { error_at(&parser_curr, "Expected create/update/delete"); return make_error_stmt(); }
        
        allow_implicit_call = 0;
        s->as.file_stmt.file = parse_expr(PREC_ASSIGN);
        allow_implicit_call = 1;
        
        if (strcmp(s->as.file_stmt.action, "delete") != 0) {
            skip_newlines();
            if (match_token(TOKEN_LBRACKET)) { skip_newlines(); s->as.file_stmt.content = parse_expr(PREC_ASSIGN); skip_newlines(); consume(TOKEN_RBRACKET, "Expected ']'"); }
            else s->as.file_stmt.content = parse_expr(PREC_ASSIGN);
        } else s->as.file_stmt.content = NULL;
        return s;
    }
    if (match_token(TOKEN_IMPORT)) {
        consume(TOKEN_STRING, "Expected string path after import"); Stmt* s = make_stmt(STMT_IMPORT, line);
        s->as.import_stmt.path = ast_strdup(parser_prev.text);
        if (match_token(TOKEN_AS)) { consume(TOKEN_IDENTIFIER, "Expected module alias"); s->as.import_stmt.alias = ast_strdup(parser_prev.text); }
        else s->as.import_stmt.alias = NULL;
        return s;
    }
    
    Expr* expr = parse_expr(PREC_ASSIGN);
    if (match_token(TOKEN_EQ)) {
        if (expr->type == EXPR_VAR) {
            Stmt* s = make_stmt(STMT_ASSIGN, line); s->as.assign_stmt.name = expr->as.name; s->as.assign_stmt.value = parse_expr(PREC_ASSIGN); return s;
        } else { error_at(&parser_curr, "Invalid assignment target"); return make_error_stmt(); }
    }
    Stmt* s = make_stmt(STMT_EXPR, line); s->as.expr = expr; return s;
}

typedef struct { Chunk* chunk; } Compiler;

void compile_expr(Compiler* compiler, Expr* expr);
void compile_stmt(Compiler* compiler, Stmt* stmt);

int emit_jump(Compiler* compiler, uint8_t instruction, int line) {
    write_chunk(compiler->chunk, instruction, line);
    write_chunk(compiler->chunk, 0xff, line);
    write_chunk(compiler->chunk, 0xff, line);
    return compiler->chunk->count - 2;
}

void patch_jump(Compiler* compiler, int offset) {
    int jump = compiler->chunk->count - offset - 2;
    if (jump > 65535) {
        fprintf(stderr, "Jump offset overflow.\n");
        exit(1);
    }
    compiler->chunk->code[offset] = (jump >> 8) & 0xff;
    compiler->chunk->code[offset + 1] = jump & 0xff;
}

void emit_loop(Compiler* compiler, int loop_start, int line) {
    write_chunk(compiler->chunk, OP_LOOP, line);
    int offset = compiler->chunk->count - loop_start + 2;
    if (offset > 65535) {
        fprintf(stderr, "Loop offset overflow.\n");
        exit(1);
    }
    write_chunk(compiler->chunk, (offset >> 8) & 0xff, line);
    write_chunk(compiler->chunk, offset & 0xff, line);
}

void compile_expr(Compiler* compiler, Expr* expr) {
    switch (expr->type) {
        case EXPR_LITERAL: {
            Value val = expr->as.literal;
            if (val.type == VAL_NULL) {
                write_chunk(compiler->chunk, OP_NULL, expr->line);
            } else if (val.type == VAL_BOOL) {
                write_chunk(compiler->chunk, val.as.boolean ? OP_TRUE : OP_FALSE, expr->line);
            } else {
                int constant = add_constant(compiler->chunk, val);
                write_chunk(compiler->chunk, OP_CONSTANT, expr->line);
                write_chunk(compiler->chunk, constant, expr->line);
            }
            break;
        }
        case EXPR_VAR: {
            ObjString* name = allocate_string(expr->as.name, strlen(expr->as.name));
            int constant = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_GET_GLOBAL, expr->line);
            write_chunk(compiler->chunk, constant, expr->line);
            break;
        }
        case EXPR_BINOP: {
            compile_expr(compiler, expr->as.bin.left);
            compile_expr(compiler, expr->as.bin.right);
            switch (expr->as.bin.op) {
                case TOKEN_PLUS:      write_chunk(compiler->chunk, OP_ADD, expr->line); break;
                case TOKEN_MINUS:     write_chunk(compiler->chunk, OP_SUBTRACT, expr->line); break;
                case TOKEN_STAR:      write_chunk(compiler->chunk, OP_MULTIPLY, expr->line); break;
                case TOKEN_SLASH:     write_chunk(compiler->chunk, OP_DIVIDE, expr->line); break;
                case TOKEN_PERCENT:   write_chunk(compiler->chunk, OP_MODULO, expr->line); break;
                case TOKEN_EQEQ:      write_chunk(compiler->chunk, OP_EQUAL, expr->line); break;
                case TOKEN_BANGEQ:    write_chunk(compiler->chunk, OP_EQUAL, expr->line); write_chunk(compiler->chunk, OP_NOT, expr->line); break;
                case TOKEN_LESS:      write_chunk(compiler->chunk, OP_LESS, expr->line); break;
                case TOKEN_LESSEQ:    write_chunk(compiler->chunk, OP_GREATER, expr->line); write_chunk(compiler->chunk, OP_NOT, expr->line); break;
                case TOKEN_GREATER:   write_chunk(compiler->chunk, OP_GREATER, expr->line); break;
                case TOKEN_GREATEREQ: write_chunk(compiler->chunk, OP_LESS, expr->line); write_chunk(compiler->chunk, OP_NOT, expr->line); break;
                default: break;
            }
            break;
        }
        case EXPR_UNARY: {
            compile_expr(compiler, expr->as.unary.right);
            if (expr->as.unary.op == TOKEN_MINUS) {
                write_chunk(compiler->chunk, OP_NEGATE, expr->line);
            }
            break;
        }
        case EXPR_CALL: {
            compile_expr(compiler, expr->as.call.callee);
            for (int i = 0; i < expr->as.call.count; i++) {
                compile_expr(compiler, expr->as.call.args[i]);
            }
            write_chunk(compiler->chunk, OP_CALL, expr->line);
            write_chunk(compiler->chunk, expr->as.call.count, expr->line);
            break;
        }
        case EXPR_MEMBER: {
            compile_expr(compiler, expr->as.member.object);
            ObjString* prop = allocate_string(expr->as.member.prop, strlen(expr->as.member.prop));
            int constant = add_constant(compiler->chunk, OBJ_VAL(prop));
            write_chunk(compiler->chunk, OP_GET_PROPERTY, expr->line);
            write_chunk(compiler->chunk, constant, expr->line);
            break;
        }
        case EXPR_ARRAY_GET: {
            ObjString* name = allocate_string(expr->as.array_get.name, strlen(expr->as.array_get.name));
            int name_const = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_GET_GLOBAL, expr->line);
            write_chunk(compiler->chunk, name_const, expr->line);
            compile_expr(compiler, expr->as.array_get.index);
            write_chunk(compiler->chunk, OP_ARRAY_GET, expr->line);
            break;
        }
        case EXPR_DICT_GET: {
            ObjString* name = allocate_string(expr->as.dict_get.name, strlen(expr->as.dict_get.name));
            int name_const = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_GET_GLOBAL, expr->line);
            write_chunk(compiler->chunk, name_const, expr->line);
            ObjString* key = allocate_string(expr->as.dict_get.key, strlen(expr->as.dict_get.key));
            int key_const = add_constant(compiler->chunk, OBJ_VAL(key));
            write_chunk(compiler->chunk, OP_CONSTANT, expr->line);
            write_chunk(compiler->chunk, key_const, expr->line);
            write_chunk(compiler->chunk, OP_DICT_GET, expr->line);
            break;
        }
        case EXPR_TIME_GET:   write_chunk(compiler->chunk, OP_TIME_GET, expr->line); break;
        case EXPR_TIME_SLEEP: compile_expr(compiler, expr->as.time_sleep.ms); write_chunk(compiler->chunk, OP_TIME_SLEEP, expr->line); break;
        default: break;
    }
}

void compile_stmt(Compiler* compiler, Stmt* stmt) {
    switch (stmt->type) {
        case STMT_EXPR: compile_expr(compiler, stmt->as.expr); write_chunk(compiler->chunk, OP_POP, stmt->line); break;
        case STMT_SAY:  compile_expr(compiler, stmt->as.expr); write_chunk(compiler->chunk, OP_SAY, stmt->line); break;
        case STMT_VAR: {
            if (stmt->as.var_decl.is_get) {
                write_chunk(compiler->chunk, OP_GET_INPUT, stmt->line);
            } else {
                compile_expr(compiler, stmt->as.var_decl.initializer);
            }
            ObjString* name = allocate_string(stmt->as.var_decl.name, strlen(stmt->as.var_decl.name));
            int constant = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_DEFINE_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, constant, stmt->line);
            break;
        }
        case STMT_ASSIGN: {
            compile_expr(compiler, stmt->as.assign_stmt.value);
            ObjString* name = allocate_string(stmt->as.assign_stmt.name, strlen(stmt->as.assign_stmt.name));
            int constant = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_SET_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, constant, stmt->line);
            break;
        }
        case STMT_ARRAY: {
            for (int i = 0; i < stmt->as.arr_decl.count; i++) {
                compile_expr(compiler, stmt->as.arr_decl.elements[i]);
            }
            write_chunk(compiler->chunk, OP_ARRAY, stmt->line);
            write_chunk(compiler->chunk, stmt->as.arr_decl.count, stmt->line);
            ObjString* name = allocate_string(stmt->as.arr_decl.name, strlen(stmt->as.arr_decl.name));
            int constant = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_DEFINE_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, constant, stmt->line);
            break;
        }
        case STMT_ARRAY_SET: {
            ObjString* name = allocate_string(stmt->as.array_set.name, strlen(stmt->as.array_set.name));
            int name_const = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_GET_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, name_const, stmt->line);
            compile_expr(compiler, stmt->as.array_set.index);
            compile_expr(compiler, stmt->as.array_set.value);
            write_chunk(compiler->chunk, OP_ARRAY_SET, stmt->line);
            break;
        }
        case STMT_DICT: {
            for (int i = 0; i < stmt->as.dict_decl.count; i++) {
                ObjString* key = allocate_string(stmt->as.dict_decl.keys[i], strlen(stmt->as.dict_decl.keys[i]));
                int key_const = add_constant(compiler->chunk, OBJ_VAL(key));
                write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
                write_chunk(compiler->chunk, key_const, stmt->line);
                compile_expr(compiler, stmt->as.dict_decl.values[i]);
            }
            write_chunk(compiler->chunk, OP_DICT, stmt->line);
            write_chunk(compiler->chunk, stmt->as.dict_decl.count, stmt->line);
            ObjString* name = allocate_string(stmt->as.dict_decl.name, strlen(stmt->as.dict_decl.name));
            int constant = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_DEFINE_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, constant, stmt->line);
            break;
        }
        case STMT_DICT_SET: {
            ObjString* name = allocate_string(stmt->as.dict_set.name, strlen(stmt->as.dict_set.name));
            int name_const = add_constant(compiler->chunk, OBJ_VAL(name));
            write_chunk(compiler->chunk, OP_GET_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, name_const, stmt->line);
            ObjString* key = allocate_string(stmt->as.dict_set.key, strlen(stmt->as.dict_set.key));
            int key_const = add_constant(compiler->chunk, OBJ_VAL(key));
            write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
            write_chunk(compiler->chunk, key_const, stmt->line);
            compile_expr(compiler, stmt->as.dict_set.value);
            write_chunk(compiler->chunk, OP_DICT_SET, stmt->line);
            break;
        }
        case STMT_JOB: {
            ObjJob* job = (ObjJob*)allocate_object(sizeof(ObjJob), OBJ_JOB);
            job->name = allocate_string(stmt->as.job_decl.name, strlen(stmt->as.job_decl.name));
            job->arity = stmt->as.job_decl.param_count;
            job->params = (ObjString**)safe_alloc(sizeof(ObjString*) * job->arity);
            for (int i = 0; i < job->arity; i++) {
                job->params[i] = allocate_string(stmt->as.job_decl.params[i], strlen(stmt->as.job_decl.params[i]));
            }
            init_chunk(&job->chunk);
            job->closure = NULL;
            
            Compiler sub_compiler = {&job->chunk};
            for (int i = 0; i < stmt->as.job_decl.body_count; i++) {
                compile_stmt(&sub_compiler, stmt->as.job_decl.body[i]);
            }
            write_chunk(&job->chunk, OP_NULL, stmt->line);
            write_chunk(&job->chunk, OP_RETURN, stmt->line);
            
            int constant = add_constant(compiler->chunk, OBJ_VAL(job));
            write_chunk(compiler->chunk, OP_CLOSURE, stmt->line);
            write_chunk(compiler->chunk, constant, stmt->line);
            
            int name_const = add_constant(compiler->chunk, OBJ_VAL(job->name));
            write_chunk(compiler->chunk, OP_DEFINE_GLOBAL, stmt->line);
            write_chunk(compiler->chunk, name_const, stmt->line);
            break;
        }
        case STMT_OUT: {
            if (stmt->as.expr != NULL) {
                compile_expr(compiler, stmt->as.expr);
            } else {
                write_chunk(compiler->chunk, OP_NULL, stmt->line);
            }
            write_chunk(compiler->chunk, OP_RETURN, stmt->line);
            break;
        }
        case STMT_IF: {
            compile_expr(compiler, stmt->as.if_stmt.cond);
            int then_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, stmt->line);
            write_chunk(compiler->chunk, OP_POP, stmt->line);
            for (int i = 0; i < stmt->as.if_stmt.then_c; i++) {
                compile_stmt(compiler, stmt->as.if_stmt.then_b[i]);
            }
            int else_jump = emit_jump(compiler, OP_JUMP, stmt->line);
            patch_jump(compiler, then_jump);
            write_chunk(compiler->chunk, OP_POP, stmt->line);
            if (stmt->as.if_stmt.else_b != NULL) {
                for (int i = 0; i < stmt->as.if_stmt.else_c; i++) {
                    compile_stmt(compiler, stmt->as.if_stmt.else_b[i]);
                }
            }
            patch_jump(compiler, else_jump);
            break;
        }
        case STMT_REPEAT: {
            int loop_start = compiler->chunk->count;
            if (stmt->as.repeat_stmt.forever) {
                for (int i = 0; i < stmt->as.repeat_stmt.body_count; i++) {
                    compile_stmt(compiler, stmt->as.repeat_stmt.body[i]);
                }
                emit_loop(compiler, loop_start, stmt->line);
            } else {
                compile_expr(compiler, stmt->as.repeat_stmt.count);
                int repeat_start = compiler->chunk->count;
                write_chunk(compiler->chunk, OP_DUP, stmt->line);
                write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
                int zero_const = add_constant(compiler->chunk, make_int(0));
                write_chunk(compiler->chunk, zero_const, stmt->line);
                write_chunk(compiler->chunk, OP_GREATER, stmt->line);
                int exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, stmt->line);
                write_chunk(compiler->chunk, OP_POP, stmt->line);
                for (int i = 0; i < stmt->as.repeat_stmt.body_count; i++) {
                    compile_stmt(compiler, stmt->as.repeat_stmt.body[i]);
                }
                write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
                int one_const = add_constant(compiler->chunk, make_int(1));
                write_chunk(compiler->chunk, one_const, stmt->line);
                write_chunk(compiler->chunk, OP_SUBTRACT, stmt->line);
                emit_loop(compiler, repeat_start, stmt->line);
                patch_jump(compiler, exit_jump);
                write_chunk(compiler->chunk, OP_POP, stmt->line);
                write_chunk(compiler->chunk, OP_POP, stmt->line);
            }
            break;
        }
        case STMT_FILE: {
            compile_expr(compiler, stmt->as.file_stmt.file);
            if (stmt->as.file_stmt.content != NULL) {
                compile_expr(compiler, stmt->as.file_stmt.content);
            } else {
                write_chunk(compiler->chunk, OP_NULL, stmt->line);
            }
            int action_const = add_constant(compiler->chunk, OBJ_VAL(allocate_string(stmt->as.file_stmt.action, strlen(stmt->as.file_stmt.action))));
            write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
            write_chunk(compiler->chunk, action_const, stmt->line);
            write_chunk(compiler->chunk, OP_FILE, stmt->line);
            break;
        }
        case STMT_IMPORT: {
            ObjString* path = allocate_string(stmt->as.import_stmt.path, strlen(stmt->as.import_stmt.path));
            ObjString* alias = stmt->as.import_stmt.alias != NULL ? allocate_string(stmt->as.import_stmt.alias, strlen(stmt->as.import_stmt.alias)) : NULL;
            int path_const = add_constant(compiler->chunk, OBJ_VAL(path));
            int alias_const = add_constant(compiler->chunk, alias != NULL ? OBJ_VAL(alias) : make_null());
            write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
            write_chunk(compiler->chunk, path_const, stmt->line);
            write_chunk(compiler->chunk, OP_CONSTANT, stmt->line);
            write_chunk(compiler->chunk, alias_const, stmt->line);
            write_chunk(compiler->chunk, OP_IMPORT, stmt->line);
            break;
        }
        default: break;
    }
}

typedef enum { INTERPRET_OK, INTERPRET_COMPILE_ERROR, INTERPRET_RUNTIME_ERROR } InterpretResult;

char* value_to_string(Value val) {
    char buffer[512];
    if (val.type == VAL_NULL) strcpy(buffer, "null");
    else if (val.type == VAL_BOOL) strcpy(buffer, val.as.boolean ? "true" : "false");
    else if (val.type == VAL_INT) snprintf(buffer, sizeof(buffer), "%lld", val.as.integer);
    else if (val.type == VAL_FLOAT) snprintf(buffer, sizeof(buffer), "%g", val.as.floating);
    else if (val.type == VAL_OBJ) {
        if (val.as.obj->type == OBJ_STRING) return safe_strdup(((ObjString*)val.as.obj)->chars);
        else strcpy(buffer, "<Object>");
    }
    return safe_strdup(buffer);
}

int is_truthy(Value val) {
    if (val.type == VAL_NULL) return 0;
    if (val.type == VAL_BOOL) return val.as.boolean;
    if (val.type == VAL_INT) return val.as.integer != 0;
    if (val.type == VAL_FLOAT) return val.as.floating != 0.0;
    return 1;
}

int values_equal(Value a, Value b) {
    if (a.type != b.type) return 0; if (a.type == VAL_NULL) return 1;
    if (a.type == VAL_BOOL) return a.as.boolean == b.as.boolean;
    if (a.type == VAL_INT) return a.as.integer == b.as.integer;
    if (a.type == VAL_FLOAT) return a.as.floating == b.as.floating;
    if (a.type == VAL_OBJ) {
        if (a.as.obj->type == OBJ_STRING && b.as.obj->type == OBJ_STRING) return a.as.obj == b.as.obj;
        return a.as.obj == b.as.obj;
    }
    return 0;
}

InterpretResult run() {
    CallFrame* frame = &vm.frames[vm.frame_count - 1];
    int sentinel_frame = vm.frame_count - 1;
    
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->job->chunk.constants[READ_BYTE()])
#define READ_STRING() (READ_CONSTANT().as.obj)

#define BINARY_OP(value_type, op) \
    do { \
        if (peek(0).type != VAL_INT && peek(0).type != VAL_FLOAT) { \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        if (peek(1).type != VAL_INT && peek(1).type != VAL_FLOAT) { \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        Value b = pop(); \
        Value a = pop(); \
        double a_val = a.type == VAL_FLOAT ? a.as.floating : (double)a.as.integer; \
        double b_val = b.type == VAL_FLOAT ? b.as.floating : (double)b.as.integer; \
        int is_int = (a.type == VAL_INT && b.type == VAL_INT); \
        if (is_int && #op[0] != '/') { \
            if (strcmp(#op, "+") == 0) push(value_type(a.as.integer + b.as.integer)); \
            else if (strcmp(#op, "-") == 0) push(value_type(a.as.integer - b.as.integer)); \
            else if (strcmp(#op, "*") == 0) push(value_type(a.as.integer * b.as.integer)); \
        } else { \
            if (strcmp(#op, "/") == 0) { \
                if (b_val == 0) { runtime_error("Division by zero."); return INTERPRET_RUNTIME_ERROR; } \
            } \
            if (strcmp(#op, "+") == 0) push(make_float(a_val + b_val)); \
            else if (strcmp(#op, "-") == 0) push(make_float(a_val - b_val)); \
            else if (strcmp(#op, "*") == 0) push(make_float(a_val * b_val)); \
            else if (strcmp(#op, "/") == 0) push(make_float(a_val / b_val)); \
        } \
    } while (false)

#define BINARY_COMP_OP(op) \
    do { \
        if (peek(0).type != VAL_INT && peek(0).type != VAL_FLOAT) { \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        if (peek(1).type != VAL_INT && peek(1).type != VAL_FLOAT) { \
            runtime_error("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        Value b = pop(); \
        Value a = pop(); \
        double a_val = a.type == VAL_FLOAT ? a.as.floating : (double)a.as.integer; \
        double b_val = b.type == VAL_FLOAT ? b.as.floating : (double)b.as.integer; \
        push(make_bool(a_val op b_val)); \
    } while (false)

    for (;;) {
        if (had_runtime_error) return INTERPRET_RUNTIME_ERROR;
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_CONSTANT: push(READ_CONSTANT()); break;
            case OP_NULL:  push(make_null()); break;
            case OP_TRUE:  push(make_bool(1)); break;
            case OP_FALSE: push(make_bool(0)); break;
            case OP_POP:   pop(); break;
            case OP_DUP:   push(peek(0)); break;
            case OP_NEGATE: {
                if (peek(0).type == VAL_INT) { push(make_int(-pop().as.integer)); }
                else if (peek(0).type == VAL_FLOAT) { push(make_float(-pop().as.floating)); }
                else { runtime_error("Operand must be a number."); return INTERPRET_RUNTIME_ERROR; }
                break;
            }
            case OP_NOT: push(make_bool(!is_truthy(pop()))); break;
            case OP_ADD: {
                if ((peek(0).type == VAL_OBJ && peek(0).as.obj->type == OBJ_STRING) ||
                    (peek(1).type == VAL_OBJ && peek(1).as.obj->type == OBJ_STRING)) {
                    Value b = pop();
                    Value a = pop();
                    char* b_str = value_to_string(b);
                    char* a_str = value_to_string(a);
                    int len = strlen(a_str) + strlen(b_str);
                    char* joined = (char*)safe_alloc(len + 1);
                    strcpy(joined, a_str); 
                    strcat(joined, b_str);
                    ObjString* res = allocate_string(joined, len);
                    safe_free(joined);
                    safe_free(b_str);
                    safe_free(a_str);
                    push(OBJ_VAL(res));
                } else {
                    BINARY_OP(make_int, +);
                }
                break;
            }
            case OP_SUBTRACT: BINARY_OP(make_int, -); break;
            case OP_MULTIPLY: BINARY_OP(make_int, *); break;
            case OP_DIVIDE:   BINARY_OP(make_int, /); break;
            case OP_MODULO: {
                if (peek(0).type != VAL_INT && peek(0).type != VAL_FLOAT) {
                    runtime_error("Operands must be numbers.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (peek(1).type != VAL_INT && peek(1).type != VAL_FLOAT) {
                    runtime_error("Operands must be numbers.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value b = pop();
                Value a = pop();
                long long a_val = a.type == VAL_INT ? a.as.integer : (long long)a.as.floating;
                long long b_val = b.type == VAL_INT ? b.as.integer : (long long)b.as.floating;
                if (b_val == 0) {
                    runtime_error("Division by zero.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(make_int(a_val % b_val));
                break;
            }
            case OP_EQUAL: {
                Value b = pop(); Value a = pop();
                push(make_bool(values_equal(a, b)));
                break;
            }
            case OP_GREATER: BINARY_COMP_OP(>); break;
            case OP_LESS:    BINARY_COMP_OP(<); break;
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (!is_truthy(peek(0))) frame->ip += offset;
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = (ObjString*)READ_STRING();
                Value value = env_get(vm.env, name->chars);
                if (value.type == VAL_NULL) {
                    runtime_error("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (value.type == VAL_OBJ && value.as.obj->type == OBJ_JOB) {
                    ObjJob* job = (ObjJob*)value.as.obj;
                    if (job->arity == 0) {
                        if (vm.frame_count >= FRAMES_MAX) {
                            runtime_error("Stack overflow.");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Env* local = create_env(job->closure);
                        vm.env = local;
                        CallFrame* new_frame = &vm.frames[vm.frame_count++];
                        new_frame->job = job;
                        new_frame->ip = job->chunk.code;
                        new_frame->slots = vm.stack_top;
                        new_frame->env = local;
                        frame = new_frame;
                        break;
                    }
                }
                push(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = (ObjString*)READ_STRING();
                Value val = pop();
                env_define(vm.env, name->chars, val);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = (ObjString*)READ_STRING();
                Value val = peek(0);
                if (!env_set(vm.env, name->chars, val)) {
                    runtime_error("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_CLOSURE: {
                ObjJob* job = (ObjJob*)READ_STRING();
                ObjJob* closure = (ObjJob*)allocate_object(sizeof(ObjJob), OBJ_JOB);
                closure->name = job->name;
                closure->arity = job->arity;
                closure->params = job->params;
                closure->chunk = job->chunk;
                closure->closure = vm.env;
                push(OBJ_VAL(closure));
                break;
            }
            case OP_CALL: {
                int arg_count = READ_BYTE();
                Value callee = peek(arg_count);
                if (callee.type != VAL_OBJ || callee.as.obj->type != OBJ_JOB) {
                    runtime_error("Can only call jobs.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjJob* job = (ObjJob*)callee.as.obj;
                if (arg_count != job->arity) {
                    runtime_error("Expected %d arguments but got %d.", job->arity, arg_count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm.frame_count >= FRAMES_MAX) {
                    runtime_error("Stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                Env* local = create_env(job->closure);
                for (int i = 0; i < job->arity; i++) {
                    Value arg = peek(job->arity - 1 - i);
                    env_define(local, job->params[i]->chars, arg);
                }
                vm.env = local;
                CallFrame* new_frame = &vm.frames[vm.frame_count++];
                new_frame->job = job;
                new_frame->ip = job->chunk.code;
                new_frame->slots = vm.stack_top - arg_count - 1;
                new_frame->env = local;
                frame = new_frame;
                break;
            }
            case OP_RETURN: {
                Value result = pop();
                vm.frame_count--;
                vm.stack_top = frame->slots;
                pop_env();
                if (vm.frame_count <= sentinel_frame) {
                    if (vm.frame_count > 0) {
                        push(result);
                    }
                    return INTERPRET_OK;
                }
                push(result);
                frame = &vm.frames[vm.frame_count - 1];
                vm.env = frame->env;
                break;
            }
            case OP_ARRAY: {
                int count = READ_BYTE();
                Value arr_val = make_array();
                ObjArray* arr = (ObjArray*)arr_val.as.obj;
                arr->count = count; arr->capacity = count;
                arr->items = (Value*)safe_alloc(sizeof(Value) * count);
                for (int i = 0; i < count; i++) {
                    arr->items[i] = peek(count - 1 - i);
                }
                for (int i = 0; i < count; i++) pop();
                push(arr_val);
                break;
            }
            case OP_ARRAY_GET: {
                Value idx = pop(); Value obj = pop();
                if (obj.type != VAL_OBJ || obj.as.obj->type != OBJ_ARRAY) {
                    runtime_error("Cannot get index from non-array.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray* arr = (ObjArray*)obj.as.obj;
                if (idx.type != VAL_INT) {
                    runtime_error("Array index must be an integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (idx.as.integer < 0 || idx.as.integer >= arr->count) {
                    runtime_error("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(arr->items[idx.as.integer]);
                break;
            }
            case OP_ARRAY_SET: {
                Value val = pop(); Value idx = pop(); Value obj = pop();
                if (obj.type != VAL_OBJ || obj.as.obj->type != OBJ_ARRAY) {
                    runtime_error("Cannot set index on non-array.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray* arr = (ObjArray*)obj.as.obj;
                if (idx.type != VAL_INT) {
                    runtime_error("Array index must be integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (idx.as.integer < 0 || idx.as.integer >= arr->count) {
                    runtime_error("Array index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                arr->items[idx.as.integer] = val;
                push(val);
                break;
            }
            case OP_DICT: {
                int count = READ_BYTE();
                Value dict_val = make_dict();
                ObjDict* dict = (ObjDict*)dict_val.as.obj;
                for (int i = 0; i < count; i++) {
                    Value v = peek((count - 1 - i) * 2);
                    Value k = peek((count - 1 - i) * 2 + 1);
                    if (k.type != VAL_OBJ || k.as.obj->type != OBJ_STRING) {
                        runtime_error("Dictionary keys must be strings.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    table_set(&dict->table, (ObjString*)k.as.obj, v);
                }
                for (int i = 0; i < count * 2; i++) pop();
                push(dict_val);
                break;
            }
            case OP_DICT_GET: {
                Value key = pop(); Value obj = pop();
                if (obj.type != VAL_OBJ || obj.as.obj->type != OBJ_DICT) {
                    runtime_error("Cannot get key from non-dictionary.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjDict* dict = (ObjDict*)obj.as.obj;
                if (key.type != VAL_OBJ || key.as.obj->type != OBJ_STRING) {
                    runtime_error("Dictionary keys must be strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value val;
                if (table_get(&dict->table, (ObjString*)key.as.obj, &val)) push(val);
                else push(make_null());
                break;
            }
            case OP_DICT_SET: {
                Value val = pop(); Value key = pop(); Value obj = pop();
                if (obj.type != VAL_OBJ || obj.as.obj->type != OBJ_DICT) {
                    runtime_error("Cannot set key on non-dictionary.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjDict* dict = (ObjDict*)obj.as.obj;
                if (key.type != VAL_OBJ || key.as.obj->type != OBJ_STRING) {
                    runtime_error("Dictionary keys must be strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                table_set(&dict->table, (ObjString*)key.as.obj, val);
                push(val);
                break;
            }
            case OP_GET_PROPERTY: {
                ObjString* prop = (ObjString*)READ_STRING(); Value obj = pop();
                if (obj.type != VAL_OBJ || obj.as.obj->type != OBJ_MODULE) {
                    runtime_error("Cannot access property of non-module.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjModule* mod = (ObjModule*)obj.as.obj;
                Value value = env_get(mod->env, prop->chars);
                if (value.type == VAL_OBJ && value.as.obj->type == OBJ_JOB) {
                    ObjJob* job = (ObjJob*)value.as.obj;
                    if (job->arity == 0) {
                        if (vm.frame_count >= FRAMES_MAX) {
                            runtime_error("Stack overflow.");
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Env* local = create_env(job->closure);
                        vm.env = local;
                        CallFrame* new_frame = &vm.frames[vm.frame_count++];
                        new_frame->job = job;
                        new_frame->ip = job->chunk.code;
                        new_frame->slots = vm.stack_top;
                        new_frame->env = local;
                        frame = new_frame;
                        break;
                    }
                }
                push(value);
                break;
            }
            case OP_GET_INPUT: {
                char buf[512];
                if (fgets(buf, sizeof(buf), stdin)) {
                    buf[strcspn(buf, "\r\n")] = '\0';
                    push(OBJ_VAL(allocate_string(buf, strlen(buf))));
                } else push(make_null());
                break;
            }
            case OP_SAY: {
                Value val = pop(); char* s = value_to_string(val);
                printf("%s\n", s); safe_free(s);
                break;
            }
            case OP_FILE: {
                ObjString* action = (ObjString*)pop().as.obj; Value content = pop(); Value file = pop();
                char* fname = value_to_string(file);
                if (strcmp(action->chars, "create") == 0 || strcmp(action->chars, "update") == 0) {
                    FILE* f = fopen(fname, strcmp(action->chars, "create") == 0 ? "w" : "a");
                    if (f) {
                        char* cstr = value_to_string(content);
                        fprintf(f, "%s", cstr); safe_free(cstr); fclose(f);
                    } else {
                        runtime_error("Could not open file %s.", fname); safe_free(fname);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (strcmp(action->chars, "delete") == 0) remove(fname);
                safe_free(fname);
                break;
            }
            case OP_TIME_GET:   push(make_int(get_time_ms())); break;
            case OP_TIME_SLEEP: {
                Value ms_val = pop();
                if (ms_val.type != VAL_INT && ms_val.type != VAL_FLOAT) { runtime_error("Sleep duration must be a number."); return INTERPRET_RUNTIME_ERROR; }
                long long ms = (ms_val.type == VAL_INT) ? ms_val.as.integer : (long long)ms_val.as.floating;
                if (ms > 0) sleep_ms(ms);
                push(make_null());
                break;
            }
            case OP_IMPORT: {
                Value alias_val = peek(0);
                Value path_val = peek(1);
                ObjString* path = (ObjString*)path_val.as.obj;
                
                for (int i = 0; i < vm.import_count; i++) {
                    if (strcmp(vm.import_stack[i], path->chars) == 0) {
                        runtime_error("Circular import detected: '%s'.", path->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
                
                if (vm.import_count >= vm.import_capacity) {
                    vm.import_capacity = vm.import_capacity < 8 ? 8 : vm.import_capacity * 2;
                    vm.import_stack = safe_realloc(vm.import_stack, sizeof(char*) * vm.import_capacity);
                }
                vm.import_stack[vm.import_count++] = safe_strdup(path->chars);
                
                Env* mod_env = create_env(NULL);
                run_file(path->chars, mod_env);
                
                if (had_error) {
                    runtime_error("Failed to compile imported module '%s'.", path->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                vm.import_count--;
                safe_free(vm.import_stack[vm.import_count]);
                
                push(OBJ_VAL((Object*)mod_env));
                ObjModule* mod = (ObjModule*)allocate_object(sizeof(ObjModule), OBJ_MODULE);
                pop();
                
                mod->env = mod_env; Value mod_val = OBJ_VAL(mod);
                if (alias_val.type != VAL_NULL) {
                    ObjString* alias = (ObjString*)alias_val.as.obj;
                    env_define(vm.env, alias->chars, mod_val);
                } else {
                    char* last = strrchr(path->chars, '/'); char* base = last ? last + 1 : path->chars;
                    char* dot = strchr(base, '.');
                    if (dot) {
                        int len = dot - base; char* a = safe_alloc(len + 1);
                        strncpy(a, base, len); env_define(vm.env, a, mod_val); safe_free(a);
                    } else {
                        env_define(vm.env, base, mod_val);
                    }
                }
                
                pop();
                pop();
                break;
            }
            default: break;
        }
    }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
#undef BINARY_COMP_OP
}

void run_script(const char* source, Env* env) {
    had_error = 0;
    init_lexer(source);
    advance_parser();
    
    Stmt** stmts = NULL; int stmt_count = 0;
    skip_newlines();
    while (parser_curr.type != TOKEN_EOF) {
        stmts = AST_REALLOC_ARRAY(stmts, Stmt*, stmt_count, stmt_count + 1);
        stmts[stmt_count++] = parse_statement();
        if (had_error) synchronize();
        skip_newlines();
    }
    
    if (had_error) return;
    
    ObjJob* script_job = (ObjJob*)allocate_object(sizeof(ObjJob), OBJ_JOB);
    script_job->name = allocate_string("main_script", 11);
    script_job->arity = 0;
    script_job->params = NULL;
    init_chunk(&script_job->chunk);
    script_job->closure = env;
    
    Compiler compiler = {&script_job->chunk};
    for (int i = 0; i < stmt_count; i++) {
        compile_stmt(&compiler, stmts[i]);
    }
    write_chunk(&script_job->chunk, OP_NULL, 1);
    write_chunk(&script_job->chunk, OP_RETURN, 1);
    
    Env* saved_env = vm.env;
    int saved_frame_count = vm.frame_count;
    Value* saved_stack_top = vm.stack_top;
    
    vm.env = env;
    CallFrame* frame = &vm.frames[vm.frame_count++];
    frame->job = script_job;
    frame->ip = script_job->chunk.code;
    frame->slots = vm.stack_top;
    frame->env = env;
    
    push(OBJ_VAL(script_job));
    
    run();
    
    vm.env = saved_env;
    if (saved_frame_count == 0) {
        vm.frame_count = 0;
        vm.stack_top = vm.stack;
    } else {
        vm.frame_count = saved_frame_count;
        vm.stack_top = saved_stack_top;
    }
}

void run_file(const char* path, Env* env) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: Could not open file %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    
    char* source = safe_alloc(size + 1);
    size_t read_bytes = fread(source, 1, size, f); source[read_bytes] = '\0'; fclose(f);
    
    Lexer old_lexer = lexer; EasecToken old_curr = parser_curr; EasecToken old_prev = parser_prev;
    run_script(source, env);
    lexer = old_lexer; parser_curr = old_curr; parser_prev = old_prev;
    
    safe_free(source);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("Usage: %s <filename.easec>\n", argv[0]); return 1; }
    
    init_vm();
    Env* global_env = create_env(NULL);
    
    run_file(argv[1], global_env);
    
    pop_env();
    vm.gc_paused = 0; vm.next_gc = 0;
    
    Object* curr = vm.objects;
    while(curr) { curr->is_constant = 0; curr = curr->next; }
    gc_collect();
    
    safe_free(vm.env_stack);
    free_table(&vm.strings);
    for (int i = 0; i < vm.import_count; i++) {
        safe_free(vm.import_stack[i]);
    }
    safe_free(vm.import_stack);
    
    free_ast();
    
    return 0;
}