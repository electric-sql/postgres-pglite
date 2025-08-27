#pragma once


#define STROPS_BUF 1024

char tmpstr[STROPS_BUF];

static
void mkdirp(const char *p) {
	if (!mkdir(p, 0700)) {
		fprintf(stderr, "# no '%s' directory, creating one ...\n", p);
	}
}

#if FIXME
#warning "some FIXME are used"
bool startswith(const char *str, const char *prefix) {
    // Check if the length of prefix is greater than the string
    if (strlen(prefix) > strlen(str)) {
        return false;
    }
    // Compare the beginning of the string with the prefix
    return strncmp(str, prefix, strlen(prefix)) == 0;
}
#endif

// Safe concatenation: tolerate NULL inputs and cap to STROPS_BUF
static inline void
strconcat(char *p, const char *head, const char *tail) {
    size_t len = 0;

    if (head && head[0] && len < STROPS_BUF) {
        size_t l = strnlen(head, STROPS_BUF - len);
        memcpy(p + len, head, l);
        len += l;
    }

    if (tail && tail[0] && len < STROPS_BUF) {
        size_t l = strnlen(tail, STROPS_BUF - len);
        memcpy(p + len, tail, l);
        len += l;
    }

    if (len >= STROPS_BUF)
        len = STROPS_BUF - 1;
    p[len] = '\0';
}

// getenv fallback that never returns NULL
static inline char *
setdefault(const char* key, const char *value) {
    // Set only if not already set
    if (value)
        setenv(key, value, 0);
    const char *res = getenv(key);
    if (!res)
        res = (value ? value : "");
    return strdup(res);
}

static inline char *
strcat_alloc(const char *head, const char *tail) {
    char buf[STROPS_BUF];
    strconcat(&buf[0], head, tail);
    return strdup((const char *)&buf[0]);
}

static inline void
mksub_dir(const char *dir, const char *sub) {
    char buf[STROPS_BUF];
    strconcat(&buf[0], dir, sub);
    mkdirp(&buf[0]);
}


#if PGDEBUG
static void
print_bits(size_t const size, void const * const ptr)
{
    unsigned char *b = (unsigned char*) ptr;
    unsigned char byte;
    int i, j;

    for (i = size-1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
            byte = (b[i] >> j) & 1;
            printf("%u", byte);
        }
    }
    puts("");
}
#endif // PGDEBUG




