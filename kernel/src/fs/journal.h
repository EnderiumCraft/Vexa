#ifndef VEXA_FS_JOURNAL_H
#define VEXA_FS_JOURNAL_H

/*
 * The journal of an ext3 (or ext4) file system: JBD2, Linux's format, kept
 * in a file system's journal inode (fs/journal.c). ext2.c gives it the
 * journal's blocks on the disk; it replays what an interrupted write left
 * there, and writes each transaction (the metadata blocks one operation
 * changed) to the journal before they go to their places.
 */
#include <stdbool.h>
#include <stdint.h>

struct block_device;
struct journal;

/* Opens the journal in `blocks` (its disk blocks, in order); replays it if
 * it holds committed transactions (`*replayed` says so). NULL with an error
 * in `*error`: -VX_EINVAL for a journal Vexa can't write (features it doesn't
 * know: the file system is mounted read-only then, if it needn't recovery). */
struct journal *journal_open(struct block_device *device, uint32_t block_size, uint32_t *blocks,
                             uint32_t count, bool *replayed, int *error);
/* How many blocks a transaction can hold. */
uint32_t journal_capacity(struct journal *journal);
/* Writes a transaction: the blocks numbered `numbers`, with contents
 * `data[i]` (block_size bytes each): to the journal, then a commit block,
 * then to their places; then the journal is empty again. */
int journal_commit(struct journal *journal, const uint32_t *numbers, uint8_t *const *data,
                   uint32_t count);

#endif
