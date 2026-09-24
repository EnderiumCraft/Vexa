#include <vexa/fs.h>
#include <vexa/vfs.h>

extern const struct filesystem_type tmpfs_type;
extern const struct filesystem_type devfs_type;
extern const struct filesystem_type ext2_type;

void fs_init(void) {
    vfs_register_filesystem(&tmpfs_type);
    vfs_register_filesystem(&devfs_type);
    vfs_register_filesystem(&ext2_type);
}
