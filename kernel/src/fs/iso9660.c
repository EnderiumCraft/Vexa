#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * ISO 9660, the CD file system, read-only, with the Rock Ridge extensions
 * (POSIX names, permissions, symbolic links and times), as xorriso writes
 * them. Vexa's boot CD holds the Linux subsystem's files; reading them from
 * there instead of the initramfs keeps them out of memory until they're used.
 *
 * Directory records never cross a 2048-byte sector: a zero length byte means
 * the rest of the sector is padding. Rock Ridge data lives in each record's
 * System Use area (after its ISO name), and can continue elsewhere ("CE").
 */

#define SECTOR 2048
#define VOLUME_DESCRIPTORS 16 /* The first one's sector. */
#define MAX_NAME 255

/* Record flags. */
#define RECORD_DIRECTORY 0x02

/* POSIX file types (Rock Ridge "PX" modes). */
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFLNK 0120000

struct iso {
    struct block_device *device;
    struct mount *mount;
    uint32_t skip; /* Bytes before the Rock Ridge entries in each System Use area. */
    bool rock_ridge;
    struct iso_node *open_nodes;
};

struct iso_node {
    struct vnode vnode;
    uint64_t key;   /* Directories: their extent; others: their record's position. */
    uint32_t extent;
    char *link;     /* Symbolic links: the target. */
    struct iso_node *next;
};

/* What a directory record says, with Rock Ridge applied. */
struct entry {
    uint64_t position; /* Of the record, in bytes from the start of the disc. */
    uint32_t extent, size;
    uint32_t type;     /* VX_TYPE_* */
    uint32_t mode;
    int64_t modified;
    bool hidden;       /* A relocated directory's placeholder ("RE"). */
    bool dot, dotdot;
    char name[MAX_NAME + 1];
    size_t name_length;
    char link[1024];
    size_t link_length;
    bool link_joined;
};

static const struct vnode_ops iso_ops;

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* The 7-byte date of directory records and Rock Ridge "TF" entries. */
static int64_t record_time(const uint8_t *t) {
    if (t[1] < 1 || t[1] > 12 || t[2] < 1) {
        return 0;
    }
    int64_t seconds = days_from_civil(1900 + t[0], t[1], t[2]) * 86400 + t[3] * 3600 +
                      t[4] * 60 + t[5];
    return seconds - (int8_t)t[6] * 15 * 60; /* Offset from UTC, in 15 minutes. */
}

static int read_bytes(struct iso *fs, uint64_t offset, void *buffer, size_t size) {
    return block_read_bytes(fs->device, offset, buffer, size);
}

/* Rock Ridge entries in `area`. Returns true if a "CE" says to go on reading
 * elsewhere (and where). */
static bool rock_ridge(struct iso *fs, const uint8_t *area, size_t length, struct entry *e,
                       uint64_t *next, uint32_t *next_length, bool *name_done, bool *link_done) {
    bool more = false;
    size_t at = 0;
    while (at + 4 <= length) {
        const uint8_t *p = area + at;
        uint8_t size = p[2];
        if (size < 4 || at + size > length) {
            break;
        }
        if (p[0] == 'P' && p[1] == 'X' && size >= 36) {
            uint32_t mode = le32(p + 4);
            e->mode = mode & 07777;
            e->type = (mode & S_IFMT) == S_IFDIR ? VX_TYPE_DIRECTORY
                      : (mode & S_IFMT) == S_IFLNK ? VX_TYPE_SYMLINK
                                                   : VX_TYPE_FILE;
        } else if (p[0] == 'N' && p[1] == 'M' && size >= 5 && !*name_done) {
            uint8_t flags = p[4];
            if (flags & 0x6) { /* CURRENT or PARENT: keep the ISO name. */
                *name_done = true;
            } else {
                size_t n = size - 5;
                if (e->name_length + n > MAX_NAME) {
                    n = MAX_NAME - e->name_length;
                }
                memcpy(e->name + e->name_length, p + 5, n);
                e->name_length += n;
                e->name[e->name_length] = '\0';
                *name_done = !(flags & 0x1); /* CONTINUE: the name goes on. */
            }
        } else if (p[0] == 'S' && p[1] == 'L' && size >= 5 && !*link_done) {
            /* Components (flags, length, text), joined with '/'. */
            size_t c = 5;
            while (c + 2 <= size && c + 2 + p[c + 1] <= size) {
                uint8_t flags = p[c], n = p[c + 1];
                const char *text = flags & 0x2 ? "." : flags & 0x4 ? ".." : (const char *)p + c + 2;
                size_t text_length = flags & 0x2 ? 1 : flags & 0x4 ? 2 : n;
                if (flags & 0x8) { /* The root. */
                    text = "/";
                    text_length = 1;
                    e->link_length = 0;
                } else if (e->link_length && !e->link_joined &&
                           e->link[e->link_length - 1] != '/') {
                    e->link[e->link_length++] = '/';
                }
                if (e->link_length + text_length < sizeof(e->link) - 1) {
                    memcpy(e->link + e->link_length, text, text_length);
                    e->link_length += text_length;
                }
                e->link_joined = flags & 0x1; /* This component goes on in the next. */
                c += 2 + n;
            }
            *link_done = !(p[4] & 0x1);
            e->link[e->link_length] = '\0';
        } else if (p[0] == 'T' && p[1] == 'F' && size >= 5) {
            uint8_t flags = p[4];
            size_t stamp = flags & 0x80 ? 17 : 7;
            size_t field = 5;
            if (flags & 0x1) { /* Creation time comes first. */
                field += stamp;
            }
            if ((flags & 0x2) && stamp == 7 && field + 7 <= size) {
                e->modified = record_time(p + field);
            }
        } else if (p[0] == 'R' && p[1] == 'E') {
            e->hidden = true;
        } else if (p[0] == 'C' && p[1] == 'L' && size >= 12) {
            e->extent = le32(p + 4); /* A relocated directory lives there. */
            e->type = VX_TYPE_DIRECTORY;
        } else if (p[0] == 'C' && p[1] == 'E' && size >= 28) {
            *next = (uint64_t)le32(p + 4) * SECTOR + le32(p + 12);
            *next_length = le32(p + 20);
            more = true;
        } else if (p[0] == 'S' && p[1] == 'T') {
            break;
        }
        at += size;
    }
    (void)fs;
    return more;
}

/* Decodes the directory record at `record` (which is `position` bytes into
 * the disc). */
static void parse_record(struct iso *fs, const uint8_t *record, uint64_t position, struct entry *e) {
    memset(e, 0, sizeof(*e));
    e->position = position;
    e->extent = le32(record + 2);
    e->size = le32(record + 10);
    e->modified = record_time(record + 18);
    e->type = record[25] & RECORD_DIRECTORY ? VX_TYPE_DIRECTORY : VX_TYPE_FILE;
    e->mode = e->type == VX_TYPE_DIRECTORY ? 0555 : 0444;
    uint8_t id_length = record[32];
    const uint8_t *id = record + 33;
    e->dot = id_length == 1 && id[0] == 0;
    e->dotdot = id_length == 1 && id[0] == 1;

    bool name_done = false, link_done = false;
    size_t area = 33 + id_length + (id_length % 2 == 0 ? 1 : 0);
    if (fs->rock_ridge && area + fs->skip < record[0]) {
        uint64_t next = 0;
        uint32_t next_length = 0;
        bool more = rock_ridge(fs, record + area + fs->skip, record[0] - area - fs->skip, e,
                               &next, &next_length, &name_done, &link_done);
        for (int hops = 0; more && hops < 8 && next_length && next_length <= SECTOR; hops++) {
            uint8_t *buffer = kmalloc(next_length);
            if (!buffer || read_bytes(fs, next, buffer, next_length)) {
                kfree(buffer);
                break;
            }
            more = rock_ridge(fs, buffer, next_length, e, &next, &next_length, &name_done,
                              &link_done);
            kfree(buffer);
        }
    }
    if (e->name_length == 0 && !e->dot && !e->dotdot) {
        /* A plain ISO name: drop the version (";1") and a trailing dot. */
        size_t n = id_length;
        for (size_t i = 0; i < id_length; i++) {
            if (id[i] == ';') {
                n = i;
                break;
            }
        }
        if (n > 0 && id[n - 1] == '.') {
            n--;
        }
        memcpy(e->name, id, n);
        e->name_length = n;
        e->name[n] = '\0';
    }
}

/* Calls visit() for each record of the directory at `extent` (`size`
 * bytes), until it returns true. `cookie` (optional) is where to start and
 * is left after the record visit() stopped at. */
typedef bool (*record_visitor)(struct iso *fs, const uint8_t *record, uint64_t position,
                               void *arg);

static int walk(struct iso *fs, uint32_t extent, uint32_t size, uint64_t *cookie,
                record_visitor visit, void *arg) {
    uint8_t *sector = kmalloc(SECTOR);
    if (!sector) {
        return -VX_ENOMEM;
    }
    uint64_t at = cookie ? *cookie : 0;
    uint64_t loaded = UINT64_MAX;
    int result = 0;
    while (at < size) {
        uint64_t base = at & ~(uint64_t)(SECTOR - 1);
        if (base != loaded) {
            if (read_bytes(fs, (uint64_t)extent * SECTOR + base, sector, SECTOR)) {
                result = -VX_EIO;
                break;
            }
            loaded = base;
        }
        uint32_t offset = (uint32_t)(at - base);
        uint8_t length = sector[offset];
        if (length == 0 || offset + length > SECTOR || length < 34) {
            at = base + SECTOR; /* Padding to the end of the sector. */
            continue;
        }
        uint64_t position = (uint64_t)extent * SECTOR + at;
        at += length;
        if (visit(fs, sector + offset, position, arg)) {
            result = 1;
            break;
        }
    }
    if (cookie) {
        *cookie = at;
    }
    kfree(sector);
    return result;
}

/* ---- Vnodes ---- */

static struct iso *fs_of(struct vnode *vnode) {
    return vnode->data;
}

static struct iso_node *node_of(struct vnode *vnode) {
    return (struct iso_node *)vnode;
}

static int get_node(struct iso *fs, const struct entry *e, struct vnode **out) {
    uint64_t key = e->type == VX_TYPE_DIRECTORY ? (uint64_t)e->extent << 1
                                                : (e->position << 1) | 1;
    for (struct iso_node *node = fs->open_nodes; node; node = node->next) {
        if (node->key == key) {
            vnode_ref(&node->vnode);
            *out = &node->vnode;
            return 0;
        }
    }
    struct iso_node *node = kzalloc(sizeof(*node));
    if (!node) {
        return -VX_ENOMEM;
    }
    vnode_init(&node->vnode, fs->mount, e->type, &iso_ops);
    node->key = key;
    node->extent = e->extent;
    node->vnode.inode = key;
    node->vnode.size = e->size;
    node->vnode.links = e->type == VX_TYPE_DIRECTORY ? 2 : 1;
    node->vnode.mode = e->mode;
    node->vnode.modified = e->modified;
    node->vnode.data = fs;
    if (e->type == VX_TYPE_SYMLINK) {
        node->link = kmalloc(e->link_length + 1);
        if (!node->link) {
            kfree(node);
            return -VX_ENOMEM;
        }
        memcpy(node->link, e->link, e->link_length + 1);
        node->vnode.size = e->link_length;
    }
    node->next = fs->open_nodes;
    fs->open_nodes = node;
    *out = &node->vnode;
    return 0;
}

static void iso_release(struct vnode *vnode) {
    struct iso *fs = fs_of(vnode);
    struct iso_node *node = node_of(vnode);
    for (struct iso_node **link = &fs->open_nodes; *link; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            break;
        }
    }
    kfree(node->link);
    kfree(node);
}

/* ---- Directories ---- */

struct find {
    const char *name;
    size_t length;
    bool dotdot;
    struct entry *found;
};

static bool find_visitor(struct iso *fs, const uint8_t *record, uint64_t position, void *arg) {
    struct find *f = arg;
    parse_record(fs, record, position, f->found);
    if (f->dotdot) {
        return f->found->dotdot;
    }
    return !f->found->hidden && !f->found->dot && !f->found->dotdot &&
           f->found->name_length == f->length && memcmp(f->found->name, f->name, f->length) == 0;
}

static int iso_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct iso *fs = fs_of(dir);
    struct iso_node *node = node_of(dir);
    if (length == 1 && name[0] == '.') {
        vnode_ref(dir);
        *out = dir;
        return 0;
    }
    struct entry *e = kmalloc(sizeof(*e));
    if (!e) {
        return -VX_ENOMEM;
    }
    struct find f = {name, length, length == 2 && name[0] == '.' && name[1] == '.', e};
    int result = walk(fs, node->extent, (uint32_t)dir->size, NULL, find_visitor, &f);
    if (result == 1) {
        if (f.dotdot) {
            e->type = VX_TYPE_DIRECTORY;
        }
        result = get_node(fs, e, out);
    } else if (result == 0) {
        result = -VX_ENOENT;
    }
    kfree(e);
    return result;
}

struct next_entry {
    struct entry *e;
};

static bool next_visitor(struct iso *fs, const uint8_t *record, uint64_t position, void *arg) {
    struct next_entry *n = arg;
    parse_record(fs, record, position, n->e);
    return !n->e->hidden && !n->e->dot && !n->e->dotdot;
}

static int iso_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *out) {
    struct iso *fs = fs_of(dir);
    struct entry *e = kmalloc(sizeof(*e));
    if (!e) {
        return -VX_ENOMEM;
    }
    struct next_entry n = {e};
    int result = walk(fs, node_of(dir)->extent, (uint32_t)dir->size, cookie, next_visitor, &n);
    if (result == 1) {
        out->inode = e->type == VX_TYPE_DIRECTORY ? (uint64_t)e->extent << 1
                                                  : (e->position << 1) | 1;
        out->type = e->type;
        size_t n_length = e->name_length < sizeof(out->name) - 1 ? e->name_length
                                                                 : sizeof(out->name) - 1;
        memcpy(out->name, e->name, n_length);
        out->name[n_length] = '\0';
        out->name_length = (uint32_t)n_length;
    }
    kfree(e);
    return result;
}

/* ---- Files ---- */

static int64_t iso_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct iso_node *node = node_of(vnode);
    if (offset >= vnode->size) {
        return 0;
    }
    if (size > vnode->size - offset) {
        size = vnode->size - offset;
    }
    if (vnode->type == VX_TYPE_SYMLINK) {
        memcpy(buffer, node->link + offset, size);
        return (int64_t)size;
    }
    int error = read_bytes(fs_of(vnode), (uint64_t)node->extent * SECTOR + offset, buffer, size);
    return error ? error : (int64_t)size;
}

static void iso_statfs(struct mount *mount, uint64_t *total, uint64_t *free) {
    struct iso *fs = mount->data;
    *total = block_size_bytes(fs->device);
    *free = 0;
}

static const struct vnode_ops iso_ops = {
    .lookup = iso_lookup,
    .read_dir = iso_read_dir,
    .read = iso_read,
    .statfs = iso_statfs,
    .release = iso_release,
};

/* ---- Mounting ---- */

/* The root directory's "." record says whether there's Rock Ridge ("SP"),
 * and how many bytes of each System Use area come before its entries. */
static bool sp_visitor(struct iso *fs, const uint8_t *record, uint64_t position, void *arg) {
    (void)fs, (void)position;
    uint8_t id_length = record[32];
    size_t area = 33 + id_length + (id_length % 2 == 0 ? 1 : 0);
    const uint8_t *p = record + area;
    if (area + 7 <= record[0] && p[0] == 'S' && p[1] == 'P' && p[4] == 0xbe && p[5] == 0xef) {
        *(uint32_t *)arg = p[6] + 1; /* +1: found. */
    }
    return true; /* Only the first record. */
}

static int iso_mount(struct mount *mount, struct block_device *device) {
    if (!device) {
        return -VX_EINVAL;
    }
    uint8_t *pvd = kmalloc(SECTOR);
    struct iso *fs = kzalloc(sizeof(*fs));
    if (!pvd || !fs) {
        kfree(pvd);
        kfree(fs);
        return -VX_ENOMEM;
    }
    int error = -VX_EINVAL;
    /* The volume descriptors: find the primary one (type 1). */
    for (uint32_t sector = VOLUME_DESCRIPTORS; sector < VOLUME_DESCRIPTORS + 32; sector++) {
        if (block_read_bytes(device, (uint64_t)sector * SECTOR, pvd, SECTOR) ||
            memcmp(pvd + 1, "CD001", 5) != 0 || pvd[0] == 255) {
            break;
        }
        if (pvd[0] == 1 && (pvd[128] | pvd[129] << 8) == SECTOR) {
            error = 0;
            break;
        }
    }
    if (error) {
        kfree(pvd);
        kfree(fs);
        return error;
    }
    fs->device = device;
    fs->mount = mount;
    mount->read_only = true;
    mount->data = fs;
    struct entry *root = kmalloc(sizeof(*root));
    if (!root) {
        kfree(pvd);
        kfree(fs);
        return -VX_ENOMEM;
    }
    parse_record(fs, pvd + 156, (uint64_t)VOLUME_DESCRIPTORS * SECTOR + 156, root);
    uint32_t sp = 0;
    walk(fs, root->extent, root->size, NULL, sp_visitor, &sp);
    if (sp) {
        fs->rock_ridge = true;
        fs->skip = sp - 1;
    }
    root->type = VX_TYPE_DIRECTORY;
    root->mode = 0555;
    error = get_node(fs, root, &mount->root);
    kfree(root);
    kfree(pvd);
    if (error) {
        kfree(fs);
        return error;
    }
    kprintf("[iso9660] %s: %s\n", device->name,
            fs->rock_ridge ? "with Rock Ridge" : "plain ISO 9660 names");
    return 0;
}

const struct filesystem_type iso9660_type = {
    .name = "iso9660",
    .mount = iso_mount,
};

/* True if the device holds an ISO 9660 file system. */
bool iso9660_probe(struct block_device *device) {
    uint8_t id[6];
    return block_read_bytes(device, (uint64_t)VOLUME_DESCRIPTORS * SECTOR, id, 6) == 0 &&
           memcmp(id + 1, "CD001", 5) == 0;
}
