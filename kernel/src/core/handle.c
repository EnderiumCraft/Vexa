#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/spinlock.h>

struct handle_table {
    struct spinlock lock;
    struct object *objects[HANDLE_MAX];
    uint32_t rights[HANDLE_MAX];
};

struct handle_table *handle_table_create(void) {
    return kzalloc(sizeof(struct handle_table));
}

void handle_table_destroy(struct handle_table *table) {
    for (int i = 0; i < HANDLE_MAX; i++) {
        if (table->objects[i]) {
            object_put(table->objects[i]);
        }
    }
    kfree(table);
}

int handle_add(struct handle_table *table, struct object *object, uint32_t rights) {
    uint64_t flags = spin_lock_irqsave(&table->lock);
    for (int i = 0; i < HANDLE_MAX; i++) {
        if (!table->objects[i]) {
            table->objects[i] = object;
            table->rights[i] = rights;
            spin_unlock_irqrestore(&table->lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return -VX_EMFILE;
}

struct object *handle_get(struct handle_table *table, int handle, const struct object_type *type,
                          uint32_t rights, int *error) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        *error = -VX_EBADF;
        return NULL;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    struct object *object = table->objects[handle];
    if (!object || object->type != type) {
        *error = -VX_EBADF;
        object = NULL;
    } else if ((table->rights[handle] & rights) != rights) {
        *error = -VX_EACCES;
        object = NULL;
    } else {
        object_ref(object); /* Stays valid even if another thread closes the handle. */
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return object;
}

int handle_close(struct handle_table *table, int handle) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        return -VX_EBADF;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    struct object *object = table->objects[handle];
    table->objects[handle] = NULL;
    spin_unlock_irqrestore(&table->lock, flags);
    if (!object) {
        return -VX_EBADF;
    }
    object_put(object);
    return 0;
}
