#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

int index_load(Index *index) {
    memset(index, 0, sizeof(Index));
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        char hash_hex[HASH_HEX_SIZE + 1];
        if (fscanf(f, "%o %64s %lu %u %511s\n", 
                  &e->mode, hash_hex, &e->mtime_sec, &e->size, e->path) != 5) break;
        hex_to_hash(hash_hex, &e->hash);
        index->count++;
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    char temp_path[] = ".pes/index_tmp";
    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;
    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &index->entries[i];
        char hash_hex[65];
        hash_to_hex(&e->hash, hash_hex);
        fprintf(f, "%o %s %lu %u %s\n", e->mode, hash_hex, (unsigned long)e->mtime_sec, e->size, e->path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(temp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    void *data = malloc(st.st_size);
    fread(data, 1, st.st_size, f);
    fclose(f);
    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data); return -1;
    }
    free(data);
    IndexEntry *e = index_find(index, path);
    if (!e) {
        e = &index->entries[index->count++];
        strncpy(e->path, path, sizeof(e->path)-1);
    }
    e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->hash = id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;
    qsort(index->entries, index->count, sizeof(IndexEntry), compare_index_entries);
    return index_save(index);
}

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    (void)index; (void)path; return 0;
}

// THIS IS THE MISSING FUNCTION
int index_status(const Index *index) {
    printf("Staged changes:\n");
    if (index->count == 0) printf("  (nothing to show)\n");
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
    }
    return 0;
}