#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO IMPLEMENTATION ─────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header
    const char *type_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int header_len = sprintf(header, "%s %zu", type_str, len) + 1; // +1 for \0

    // 2. Combine header + data
    size_t full_len = header_len + len;
    uint8_t *full_obj = malloc(full_len);
    memcpy(full_obj, header, header_len);
    memcpy(full_obj + header_len, data, len);

    // 3. Hash full object and check deduplication
    compute_hash(full_obj, full_len, id_out);
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // 4. Create shard directory
    char path[512], dir[256];
    object_path(id_out, path, sizeof(path));
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    // 5. Write to temp file then rename (Atomic Write Pattern)
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/temp_%s", dir, hex + 2);
    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full_obj); return -1; }
    
    write(fd, full_obj, full_len);
    fsync(fd);
    close(fd);

    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // 6. Sync directory
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    free(full_obj);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t full_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buffer = malloc(full_len);
    if (fread(buffer, 1, full_len, f) != full_len) {
        fclose(f); free(buffer); return -1;
    }
    fclose(f);

    // Verify Integrity
    ObjectID actual_id;
    compute_hash(buffer, full_len, &actual_id);
    if (memcmp(id->hash, actual_id.hash, HASH_SIZE) != 0) {
        free(buffer); return -1;
    }

    // Parse Header
    char *header = (char *)buffer;
    if (strncmp(header, "blob", 4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp(header, "tree", 4) == 0) *type_out = OBJ_TREE;
    else if (strncmp(header, "commit", 6) == 0) *type_out = OBJ_COMMIT;

    char *null_byte = memchr(buffer, '\0', 64);
    size_t header_len = (null_byte - (char *)buffer) + 1;
    *len_out = full_len - header_len;

    *data_out = malloc(*len_out);
    memcpy(*data_out, buffer + header_len, *len_out);

    free(buffer);
    return 0;
}