#ifndef VEXA_OBJECT_H
#define VEXA_OBJECT_H

#include <stdint.h>

/* A kernel object a process can hold through a handle: an open file for now,
 * and later pipes, processes, sockets... Objects are reference counted and
 * destroyed when the last reference goes. */
struct object;

struct object_type {
    const char *name;
    /* Called when the last reference is dropped. May sleep. */
    void (*destroy)(struct object *object);
};

struct object {
    const struct object_type *type;
    uint32_t refs;
};

static inline void object_init(struct object *object, const struct object_type *type) {
    object->type = type;
    object->refs = 1;
}

static inline void object_ref(struct object *object) {
    __atomic_add_fetch(&object->refs, 1, __ATOMIC_RELAXED);
}

static inline void object_put(struct object *object) {
    if (__atomic_sub_fetch(&object->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        object->type->destroy(object);
    }
}

/* ---- Handle tables: a process's numbered references to objects. ---- */

#define HANDLE_MAX 256

#define HANDLE_RIGHT_READ 0x1
#define HANDLE_RIGHT_WRITE 0x2

struct handle_table;

struct handle_table *handle_table_create(void);
/* Closes every handle and frees the table. May sleep. */
void handle_table_destroy(struct handle_table *table);
/* Stores a reference to `object` (the caller's reference moves into the
 * table). Returns the handle number or -VX_EMFILE. */
int handle_add(struct handle_table *table, struct object *object, uint32_t rights);
/* Returns a new reference to the object behind `handle` if it has the given
 * type and all of `rights`; otherwise NULL with *error set (-VX_EBADF/-VX_EACCES). */
struct object *handle_get(struct handle_table *table, int handle, const struct object_type *type,
                          uint32_t rights, int *error);
int handle_close(struct handle_table *table, int handle);

#endif
