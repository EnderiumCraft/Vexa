#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/hash.h>
#include <vexa/users.h>

/* Accounts (see <vexa/users.h>). */

/* ---- Reading the files ---- */

#define LINE_FIELDS 8

/* Reads a whole small file into a new buffer (NULL if it can't). */
static char *read_all(const char *path) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    size_t size = 0, capacity = 4096;
    char *text = malloc(capacity);
    long n;
    while (text && (n = vx_read(handle, text + size, capacity - size - 1)) > 0) {
        size += (size_t)n;
        if (capacity - size < 512) {
            char *more = realloc(text, capacity *= 2);
            if (!more) {
                free(text);
            }
            text = more;
        }
    }
    vx_close(handle);
    if (text) {
        text[size] = '\0';
    }
    return text;
}

/* Splits `line` (modified) at ':' into up to LINE_FIELDS fields. */
static int split(char *line, char *fields[LINE_FIELDS]) {
    int n = 0;
    fields[n++] = line;
    for (char *c = line; *c && n < LINE_FIELDS; c++) {
        if (*c == ':') {
            *c = '\0';
            fields[n++] = c + 1;
        }
    }
    for (int i = n; i < LINE_FIELDS; i++) {
        fields[i] = "";
    }
    return n;
}

static void copy_text(char *out, size_t size, const char *in) {
    snprintf(out, size, "%s", in);
}

/* Calls `visit` for each line of `path` split into fields; it returns
 * true to stop. */
static void each_line(const char *path, bool (*visit)(char **fields, int count, void *arg),
                      void *arg) {
    char *text = read_all(path);
    if (!text) {
        return;
    }
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0] == '#' || !line[0]) {
            continue;
        }
        char *fields[LINE_FIELDS];
        int count = split(line, fields);
        if (visit(fields, count, arg)) {
            break;
        }
    }
    free(text);
}

static void user_of(char **f, struct vx_user *out) {
    memset(out, 0, sizeof(*out));
    copy_text(out->name, sizeof(out->name), f[0]);
    out->uid = (unsigned int)strtoul(f[2], NULL, 10);
    out->gid = (unsigned int)strtoul(f[3], NULL, 10);
    copy_text(out->full_name, sizeof(out->full_name), f[4]);
    copy_text(out->home, sizeof(out->home), f[5][0] ? f[5] : "/");
    copy_text(out->shell, sizeof(out->shell), f[6][0] ? f[6] : "/bin/vsh");
}

struct find {
    const char *name;
    unsigned int uid;
    struct vx_user *out;
    bool found;
};

static bool find_user(char **f, int count, void *arg) {
    struct find *find = arg;
    if (count < 4) {
        return false;
    }
    bool match = find->name ? !strcmp(f[0], find->name)
                            : (unsigned int)strtoul(f[2], NULL, 10) == find->uid;
    if (match) {
        user_of(f, find->out);
        find->found = true;
    }
    return match;
}

int vx_user_by_name(const char *name, struct vx_user *out) {
    struct find find = {name, 0, out, false};
    each_line("/etc/passwd", find_user, &find);
    return find.found ? 0 : -VX_ENOENT;
}

int vx_user_by_id(unsigned int uid, struct vx_user *out) {
    struct find find = {NULL, uid, out, false};
    each_line("/etc/passwd", find_user, &find);
    return find.found ? 0 : -VX_ENOENT;
}

struct list {
    struct vx_user *out;
    int max, count;
};

static bool list_user(char **f, int count, void *arg) {
    struct list *list = arg;
    if (count >= 4 && strtoul(f[2], NULL, 10) >= VX_USER_FIRST &&
        strtoul(f[2], NULL, 10) < 60000 && list->count < list->max) {
        user_of(f, &list->out[list->count++]);
    }
    return false;
}

int vx_users(struct vx_user *out, int max) {
    struct list list = {out, max, 0};
    each_line("/etc/passwd", list_user, &list);
    return list.count;
}

struct group_find {
    const char *name;   /* A group by name, or */
    const char *member; /* the groups of a member. */
    int id;
    unsigned int *groups;
    int max, count;
};

static bool member_of(const char *members, const char *name) {
    size_t n = strlen(name);
    for (const char *p = members; *p;) {
        const char *end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length == n && !strncmp(p, name, n)) {
            return true;
        }
        p += length + (end ? 1 : 0);
    }
    return false;
}

static bool find_group(char **f, int count, void *arg) {
    struct group_find *find = arg;
    if (count < 3) {
        return false;
    }
    if (find->name && !strcmp(f[0], find->name)) {
        find->id = (int)strtoul(f[2], NULL, 10);
        return true;
    }
    if (find->member && member_of(f[3], find->member) && find->count < find->max) {
        find->groups[find->count++] = (unsigned int)strtoul(f[2], NULL, 10);
    }
    return false;
}

int vx_group_id(const char *name) {
    struct group_find find = {name, NULL, -VX_ENOENT, NULL, 0, 0};
    each_line("/etc/group", find_group, &find);
    return find.id;
}

int vx_user_groups(const char *name, unsigned int *groups, int max) {
    struct group_find find = {NULL, name, 0, groups, max, 0};
    each_line("/etc/group", find_group, &find);
    return find.count;
}

static bool find_gid(char **f, int count, void *arg) {
    struct group_find *find = arg;
    if (count >= 3 && (int)strtoul(f[2], NULL, 10) == find->id) {
        find->count = 1;
        return true;
    }
    return false;
}

static bool group_taken(unsigned int gid) {
    struct group_find find = {NULL, NULL, (int)gid, NULL, 0, 0};
    each_line("/etc/group", find_gid, &find);
    return find.count > 0;
}

bool vx_user_is_admin(const char *name) {
    struct vx_user user;
    if (vx_user_by_name(name, &user)) {
        return false;
    }
    if (user.uid == 0 || user.gid == VX_GROUP_ADMIN) {
        return true;
    }
    unsigned int groups[VX_GROUPS_MAX];
    int n = vx_user_groups(name, groups, VX_GROUPS_MAX);
    for (int i = 0; i < n; i++) {
        if (groups[i] == VX_GROUP_ADMIN) {
            return true;
        }
    }
    return false;
}

int vx_current_user(struct vx_user *out) {
    struct vx_credentials me = {0};
    vx_credentials(NULL, &me);
    return vx_user_by_id(me.uid, out);
}

const char *vx_home(void) {
    static char home[128];
    const char *env = getenv("HOME");
    if (env && env[0]) {
        return env;
    }
    struct vx_user user;
    copy_text(home, sizeof(home), vx_current_user(&user) == 0 ? user.home : "/home");
    return home;
}

void vx_home_path(char *out, size_t size, const char *name) {
    const char *home = vx_home();
    snprintf(out, size, "%s%s%s", home, home[strlen(home) - 1] == '/' ? "" : "/", name);
}

const char *vx_home_folder(const char *name) {
    static char buffers[8][300];
    static int next;
    char *out = buffers[next++ % 8];
    vx_home_path(out, sizeof(buffers[0]), name);
    return out;
}

/* ---- SHA-256 (FIPS 180-4) ---- */

#define sha256 vx_sha256 /* <vexa/hash.h>'s, which these are. */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256 *h, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h->state[0], b = h->state[1], c = h->state[2], d = h->state[3];
    uint32_t e = h->state[4], f = h->state[5], g = h->state[6], k = h->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h->state[0] += a;
    h->state[1] += b;
    h->state[2] += c;
    h->state[3] += d;
    h->state[4] += e;
    h->state[5] += f;
    h->state[6] += g;
    h->state[7] += k;
}

static void sha256_init(struct sha256 *h) {
    static const uint32_t start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h->state, start, sizeof(start));
    h->length = 0;
    h->used = 0;
}

static void sha256_add(struct sha256 *h, const void *data, size_t size) {
    const uint8_t *p = data;
    h->length += size;
    while (size--) {
        h->block[h->used++] = *p++;
        if (h->used == 64) {
            sha256_block(h, h->block);
            h->used = 0;
        }
    }
}

static void sha256_end(struct sha256 *h, uint8_t out[32]) {
    uint64_t bits = h->length * 8;
    uint8_t pad = 0x80;
    sha256_add(h, &pad, 1);
    pad = 0;
    while (h->used != 56) {
        sha256_add(h, &pad, 1);
    }
    uint8_t length[8];
    for (int i = 0; i < 8; i++) {
        length[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_add(h, length, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h->state[i];
    }
}

void vx_sha256_init(struct vx_sha256 *h) {
    sha256_init(h);
}

void vx_sha256_add(struct vx_sha256 *h, const void *data, size_t size) {
    sha256_add(h, data, size);
}

void vx_sha256_end(struct vx_sha256 *h, uint8_t digest[32]) {
    sha256_end(h, digest);
}

/* ---- Passwords ---- */

#define HASH_PREFIX "$5v$"
#define HASH_ROUNDS 5000

static void hex(const uint8_t *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = '\0';
}

/* salt + password, stretched: each round hashes the last digest with both. */
static void digest(const char *salt, const char *password, char out[65]) {
    uint8_t d[32];
    struct sha256 h;
    sha256_init(&h);
    sha256_add(&h, salt, strlen(salt));
    sha256_add(&h, password, strlen(password));
    sha256_end(&h, d);
    for (int round = 1; round < HASH_ROUNDS; round++) {
        sha256_init(&h);
        sha256_add(&h, d, sizeof(d));
        sha256_add(&h, salt, strlen(salt));
        sha256_add(&h, password, strlen(password));
        sha256_end(&h, d);
    }
    hex(d, sizeof(d), out);
}

void vx_password_make(const char *password, char out[VX_HASH_MAX]) {
    uint8_t random[8] = {0};
    int handle = vx_open("/dev/random", VX_OPEN_READ);
    if (handle >= 0) {
        vx_read(handle, random, sizeof(random));
        vx_close(handle);
    }
    char salt[17], d[65];
    hex(random, sizeof(random), salt);
    digest(salt, password, d);
    snprintf(out, VX_HASH_MAX, HASH_PREFIX "%s$%s", salt, d);
}

bool vx_password_matches(const char *password, const char *hash) {
    if (!hash[0]) {
        return !password[0]; /* No password set. */
    }
    if (strncmp(hash, HASH_PREFIX, strlen(HASH_PREFIX))) {
        return false;
    }
    const char *salt = hash + strlen(HASH_PREFIX);
    const char *end = strchr(salt, '$');
    if (!end || end - salt > 32) {
        return false;
    }
    char s[33], d[65];
    memcpy(s, salt, (size_t)(end - salt));
    s[end - salt] = '\0';
    digest(s, password, d);
    /* Compared in full, whatever differs first. */
    unsigned char differ = (unsigned char)(strlen(end + 1) != 64);
    for (int i = 0; i < 64 && end[1 + i]; i++) {
        differ |= (unsigned char)(d[i] ^ end[1 + i]);
    }
    return !differ;
}

struct shadow_find {
    const char *name;
    char hash[VX_HASH_MAX];
    bool found;
};

static bool find_shadow(char **f, int count, void *arg) {
    struct shadow_find *find = arg;
    if (count >= 2 && !strcmp(f[0], find->name)) {
        copy_text(find->hash, sizeof(find->hash), f[1]);
        find->found = true;
        return true;
    }
    return false;
}

/* The account's hash from /etc/shadow (readable by root only). */
static bool shadow_hash(const char *name, char out[VX_HASH_MAX]) {
    struct shadow_find find = {name, "", false};
    each_line("/etc/shadow", find_shadow, &find);
    copy_text(out, VX_HASH_MAX, find.hash);
    return find.found;
}

static bool is_root(void) {
    struct vx_credentials me = {0};
    vx_credentials(NULL, &me);
    return me.euid == 0;
}

bool vx_password_check(const char *name, const char *password) {
    struct vx_user user;
    if (vx_user_by_name(name, &user)) {
        return false;
    }
    if (is_root()) {
        char hash[VX_HASH_MAX];
        return shadow_hash(name, hash) ? vx_password_matches(password, hash) : !password[0];
    }
    /* /bin/vauth (set-user-id root) reads the name and password from its
     * input and answers with its exit code. */
    int pipe[2];
    if (vx_pipe(pipe)) {
        return false;
    }
    const char *argv[] = {"vauth"};
    struct vx_spawn spawn = {.argv = argv, .argc = 1, .handles = {pipe[0], -1, -1}};
    int child = vx_spawn("/bin/vauth", &spawn);
    vx_close(pipe[0]);
    if (child < 0) {
        vx_close(pipe[1]);
        return false;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\n%s\n", name, password);
    vx_write(pipe[1], line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
    vx_close(pipe[1]);
    memset(line, 0, sizeof(line));
    long code = vx_wait(child, 0);
    vx_close(child);
    return code == 0;
}

bool vx_user_has_password(const char *name) {
    char hash[VX_HASH_MAX];
    return shadow_hash(name, hash) && hash[0];
}

bool vx_read_password(const char *prompt, char *out, size_t size) {
    fputs(prompt, stdout);
    fflush(stdout);
    int off = 0, on = 1;
    bool terminal = vx_control(0, VX_TTY_SET_ECHO, &off, sizeof(off)) == 0;
    size_t n = 0;
    char c;
    bool got = false;
    while (vx_read(0, &c, 1) == 1) {
        got = true;
        if (c == '\n' || c == '\r') {
            break;
        }
        if (n + 1 < size) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    if (terminal) {
        vx_control(0, VX_TTY_SET_ECHO, &on, sizeof(on));
        fputs("\n", stdout);
        fflush(stdout);
    }
    return got;
}

/* ---- Changing accounts ---- */

/* Rewrites one of the account files, line by line: `edit` gets each line
 * (fields split) and appends what should stay to `out`. */
struct rewrite {
    char *text;
    size_t length, capacity;
    const char *name;
    const char *value; /* What the edit puts in. */
    int what;
    bool done;
};

static void append(struct rewrite *r, const char *s) {
    size_t n = strlen(s);
    if (r->length + n + 1 > r->capacity) {
        size_t capacity = (r->length + n + 1) * 2;
        char *more = realloc(r->text, capacity);
        if (!more) {
            return;
        }
        r->text = more;
        r->capacity = capacity;
    }
    memcpy(r->text + r->length, s, n + 1);
    r->length += n;
}

static void append_fields(struct rewrite *r, char **f, int count) {
    for (int i = 0; i < count; i++) {
        if (i) {
            append(r, ":");
        }
        append(r, f[i]);
    }
    append(r, "\n");
}

static int write_back(const char *name, struct rewrite *r, unsigned int mode) {
    int error = vx_settings_write_file(name, r->text ? r->text : "", r->length);
    char path[64];
    snprintf(path, sizeof(path), "/etc/%s", name);
    vx_chmod(path, mode, 0);
    char disk[128];
    if (vx_settings_disk(disk, sizeof(disk))) {
        snprintf(path, sizeof(path), "%s/%s/%s", disk, VX_SETTINGS_DIR, name);
        vx_chmod(path, mode, 0);
    }
    free(r->text);
    return error;
}

enum { DROP, SET_HASH, SET_FULL_NAME, ADD_MEMBER, DROP_MEMBER };

static bool edit_line(char **f, int count, void *arg) {
    struct rewrite *r = arg;
    bool mine = !strcmp(f[0], r->name);
    char members[512];
    switch (r->what) {
    case DROP:
        if (mine) {
            r->done = true;
            return false;
        }
        break;
    case SET_HASH:
        if (mine) {
            f[1] = (char *)r->value;
            count = count < 3 ? 3 : count;
            r->done = true;
        }
        break;
    case SET_FULL_NAME:
        if (mine && count >= 5) {
            f[4] = (char *)r->value;
            r->done = true;
        }
        break;
    case ADD_MEMBER:
    case DROP_MEMBER:
        /* r->name is the group here; r->value the member. */
        if (mine && count >= 3) {
            members[0] = '\0';
            const char *list = count >= 4 ? f[3] : "";
            for (const char *p = list; *p;) {
                const char *end = strchr(p, ',');
                size_t length = end ? (size_t)(end - p) : strlen(p);
                bool same = length == strlen(r->value) && !strncmp(p, r->value, length);
                if (!same && length) {
                    size_t used = strlen(members);
                    snprintf(members + used, sizeof(members) - used, "%s%.*s", used ? "," : "",
                             (int)length, p);
                }
                p += length + (end ? 1 : 0);
            }
            if (r->what == ADD_MEMBER) {
                size_t used = strlen(members);
                snprintf(members + used, sizeof(members) - used, "%s%s", used ? "," : "",
                         r->value);
            }
            f[3] = members;
            count = count < 4 ? 4 : count;
            r->done = true;
        }
        break;
    }
    append_fields(r, f, count);
    return false;
}

static int rewrite_file(const char *file, const char *name, int what, const char *value,
                        unsigned int mode, bool *done) {
    struct rewrite r = {NULL, 0, 0, name, value, what, false};
    char path[64];
    snprintf(path, sizeof(path), "/etc/%s", file);
    each_line(path, edit_line, &r);
    if (done) {
        *done = r.done;
    }
    return write_back(file, &r, mode);
}

static bool name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n >= 32 || !(name[0] >= 'a' && name[0] <= 'z')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static bool text_ok(const char *text) {
    return !strchr(text, ':') && !strchr(text, '\n') && strlen(text) < 64;
}

static int add_line(const char *file, const char *line, unsigned int mode) {
    char path[64];
    snprintf(path, sizeof(path), "/etc/%s", file);
    char *old = read_all(path);
    struct rewrite r = {NULL, 0, 0, NULL, NULL, 0, false};
    if (old) {
        append(&r, old);
        if (r.length && r.text[r.length - 1] != '\n') {
            append(&r, "\n");
        }
        free(old);
    }
    append(&r, line);
    return write_back(file, &r, mode);
}

int vx_user_add(const char *name, const char *full_name, bool admin, const char *password) {
    if (!is_root()) {
        return -VX_EPERM;
    }
    struct vx_user user;
    if (!name_ok(name) || !text_ok(full_name)) {
        return -VX_EINVAL;
    }
    if (vx_user_by_name(name, &user) == 0 || vx_group_id(name) >= 0) {
        return -VX_EEXIST;
    }
    /* The next free id, the same for the account and its own group. */
    unsigned int id = VX_USER_FIRST;
    while (vx_user_by_id(id, &user) == 0 || group_taken(id)) {
        id++;
    }
    char line[512], hash[VX_HASH_MAX] = "";
    if (password && password[0]) {
        vx_password_make(password, hash);
    }
    snprintf(line, sizeof(line), "%s:x:%u:%u::\n", name, id, id);
    int error = add_line("group", line, 0644);
    snprintf(line, sizeof(line), "%s:x:%u:%u:%s:/home/%s:/bin/vsh\n", name, id, id, full_name,
             name);
    error = error ? error : add_line("passwd", line, 0644);
    snprintf(line, sizeof(line), "%s:%s:\n", name, hash);
    error = error ? error : add_line("shadow", line, 0600);
    memset(hash, 0, sizeof(hash));
    if (!error && admin) {
        error = vx_user_set_admin(name, true);
    }
    if (!error) {
        /* Their home, with the usual folders, theirs alone to change. */
        static const char *const folders[] = {"", "/Desktop", "/Documents", "/Pictures",
                                              "/Music", "/Videos"};
        for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
            snprintf(line, sizeof(line), "/home/%s%s", name, folders[i]);
            vx_mkdir(line);
            vx_chown(line, id, id, 0);
        }
    }
    return error;
}

int vx_user_remove(const char *name, bool remove_home) {
    struct vx_user user;
    if (!is_root()) {
        return -VX_EPERM;
    }
    if (vx_user_by_name(name, &user)) {
        return -VX_ENOENT;
    }
    if (user.uid < VX_USER_FIRST) {
        return -VX_EPERM; /* Not root or the system's own. */
    }
    bool done;
    int error = rewrite_file("passwd", name, DROP, NULL, 0644, &done);
    error = error ? error : rewrite_file("shadow", name, DROP, NULL, 0600, &done);
    error = error ? error : rewrite_file("group", name, DROP, NULL, 0644, &done);
    error = error ? error : rewrite_file("group", "admin", DROP_MEMBER, name, 0644, &done);
    if (!error && remove_home && !strncmp(user.home, "/home/", 6)) {
        vx_remove_tree(user.home);
    }
    return error;
}

int vx_user_set_password(const char *name, const char *password) {
    struct vx_user user;
    if (!is_root()) {
        return -VX_EPERM;
    }
    if (vx_user_by_name(name, &user)) {
        return -VX_ENOENT;
    }
    char hash[VX_HASH_MAX] = "";
    if (password && password[0]) {
        vx_password_make(password, hash);
    }
    bool done;
    int error = rewrite_file("shadow", name, SET_HASH, hash, 0600, &done);
    if (!error && !done) {
        char line[160];
        snprintf(line, sizeof(line), "%s:%s:\n", name, hash);
        error = add_line("shadow", line, 0600);
    }
    memset(hash, 0, sizeof(hash));
    return error;
}

int vx_user_set_admin(const char *name, bool admin) {
    struct vx_user user;
    if (!is_root()) {
        return -VX_EPERM;
    }
    if (vx_user_by_name(name, &user)) {
        return -VX_ENOENT;
    }
    bool done;
    return rewrite_file("group", "admin", admin ? ADD_MEMBER : DROP_MEMBER, name, 0644, &done);
}

int vx_user_set_full_name(const char *name, const char *full_name) {
    if (!is_root()) {
        return -VX_EPERM;
    }
    if (!text_ok(full_name)) {
        return -VX_EINVAL;
    }
    bool done;
    int error = rewrite_file("passwd", name, SET_FULL_NAME, full_name, 0644, &done);
    return error ? error : done ? 0 : -VX_ENOENT;
}

int vx_become_user(const struct vx_user *user) {
    struct vx_credentials set = {VX_ID_KEEP, VX_ID_KEEP, VX_ID_KEEP, user->gid, user->gid,
                                 user->gid, 0, {0}};
    set.group_count = (unsigned int)vx_user_groups(user->name, set.groups, VX_GROUPS_MAX);
    long error = vx_credentials(&set, NULL);
    if (error) {
        return (int)error;
    }
    struct vx_credentials ids = {user->uid, user->uid, user->uid, VX_ID_KEEP, VX_ID_KEEP,
                                 VX_ID_KEEP, VX_ID_KEEP, {0}};
    error = vx_credentials(&ids, NULL);
    if (error) {
        return (int)error;
    }
    setenv("HOME", user->home, 1);
    setenv("USER", user->name, 1);
    setenv("LOGNAME", user->name, 1);
    setenv("SHELL", user->shell, 1);
    return 0;
}

/* ---- <pwd.h> and <grp.h> ---- */

#include <grp.h>
#include <pwd.h>

struct passwd *getpwuid(uid_t uid) {
    static struct vx_user user;
    static struct passwd pw;
    if (vx_user_by_id(uid, &user)) {
        return NULL;
    }
    pw = (struct passwd){user.name, "x", user.uid, user.gid, user.full_name, user.home,
                         user.shell};
    return &pw;
}

struct passwd *getpwnam(const char *name) {
    struct vx_user user;
    return vx_user_by_name(name, &user) ? NULL : getpwuid(user.uid);
}

struct group_lookup {
    const char *name;
    gid_t gid;
    struct group *out;
};

static bool fill_group(char **f, int count, void *arg) {
    struct group_lookup *look = arg;
    if (count < 3) {
        return false;
    }
    bool match = look->name ? !strcmp(f[0], look->name)
                            : (gid_t)strtoul(f[2], NULL, 10) == look->gid;
    if (!match) {
        return false;
    }
    static char name[32], members_text[512];
    static char *members[33];
    copy_text(name, sizeof(name), f[0]);
    copy_text(members_text, sizeof(members_text), count >= 4 ? f[3] : "");
    int n = 0;
    for (char *p = members_text; *p && n < 32;) {
        members[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    members[n] = NULL;
    *look->out = (struct group){name, "x", (gid_t)strtoul(f[2], NULL, 10), members};
    look->name = NULL;
    look->gid = (gid_t)-1; /* (Found.) */
    return true;
}

static struct group *lookup_group(const char *name, gid_t gid) {
    static struct group group;
    struct group_lookup look = {name, gid, &group};
    each_line("/etc/group", fill_group, &look);
    return look.gid == (gid_t)-1 && !look.name ? &group : NULL;
}

struct group *getgrnam(const char *name) {
    return lookup_group(name, 0);
}

struct group *getgrgid(gid_t gid) {
    return lookup_group(NULL, gid);
}
