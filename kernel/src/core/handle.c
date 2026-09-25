#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/spinlock.h>

struct handle_table {
    struct spinlock lock;
    struct object *objects[HANDLE_MAX];
    uint32_t rights[HANDLE_MAX];
    uint32_t flags[HANDLE_MAX];
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
    return handle_add_from(table, object, rights, 0);
}

int handle_add_from(struct handle_table *table, struct object *object, uint32_t rights, int min) {
    if (min < 0) {
        min = 0;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    for (int i = min; i < HANDLE_MAX; i++) {
        if (!table->objects[i]) {
            table->objects[i] = object;
            table->rights[i] = rights;
            table->flags[i] = 0;
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

int handle_set(struct handle_table *table, int handle, struct object *object, uint32_t rights) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        object_put(object);
        return -VX_EBADF;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    struct object *old = table->objects[handle];
    table->objects[handle] = object;
    table->rights[handle] = rights;
    table->flags[handle] = 0;
    spin_unlock_irqrestore(&table->lock, flags);
    if (old) {
        object_put(old);
    }
    return handle;
}

struct handle_table *handle_table_clone(struct handle_table *table) {
    struct handle_table *copy = handle_table_create();
    if (!copy) {
        return NULL;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    for (int i = 0; i < HANDLE_MAX; i++) {
        if (table->objects[i]) {
            object_ref(table->objects[i]);
            copy->objects[i] = table->objects[i];
            copy->rights[i] = table->rights[i];
            copy->flags[i] = table->flags[i];
        }
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return copy;
}

int handle_get_flags(struct handle_table *table, int handle) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        return -VX_EBADF;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    int result = table->objects[handle] ? (int)table->flags[handle] : -VX_EBADF;
    spin_unlock_irqrestore(&table->lock, flags);
    return result;
}

int handle_set_flags(struct handle_table *table, int handle, uint32_t value) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        return -VX_EBADF;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    int result = table->objects[handle] ? 0 : -VX_EBADF;
    if (!result) {
        table->flags[handle] = value;
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return result;
}

void handle_close_on_exec(struct handle_table *table) {
    for (int i = 0; i < HANDLE_MAX; i++) {
        uint64_t flags = spin_lock_irqsave(&table->lock);
        struct object *object = NULL;
        if (table->objects[i] && (table->flags[i] & HANDLE_FLAG_CLOSE_ON_EXEC)) {
            object = table->objects[i];
            table->objects[i] = NULL;
        }
        spin_unlock_irqrestore(&table->lock, flags);
        if (object) {
            object_put(object);
        }
    }
}

struct object *handle_get_any(struct handle_table *table, int handle, uint32_t *rights) {
    if (handle < 0 || handle >= HANDLE_MAX) {
        return NULL;
    }
    uint64_t flags = spin_lock_irqsave(&table->lock);
    struct object *object = table->objects[handle];
    if (object) {
        object_ref(object);
        *rights = table->rights[handle];
    }
    spin_unlock_irqrestore(&table->lock, flags);
    return object;
}
