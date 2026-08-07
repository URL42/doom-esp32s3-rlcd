#ifndef DOOM_STORAGE_H
#define DOOM_STORAGE_H

#define STORAGE_MOUNT_POINT "/fat"

// Mount the FAT storage partition. Returns the directory Doom should use for
// savegames and config (with a trailing slash), or NULL if the mount failed --
// in which case the game still runs, it just cannot persist anything.
const char *Storage_Mount(void);

#endif // DOOM_STORAGE_H
