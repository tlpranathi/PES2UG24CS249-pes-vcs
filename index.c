#include <errno.h>
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {

            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }

    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── FIXED FUNCTIONS ─────────────────────────────────────────────────────────

// ✅ FIXED: safe load
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    char hex[HASH_HEX_SIZE + 1];

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        int rc = fscanf(f, "%o %64s %llu %u %511s\n",
                        &e->mode,
                        hex,
                        (unsigned long long *)&e->mtime_sec,
                        &e->size,
                        e->path);

        if (rc == EOF) break;
        if (rc != 5) break;

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);  // ✅ IMPORTANT

    return 0;
}

// unchanged but safe
int index_save(const Index *index) {
    Index sorted = *index;

    int cmp_entries(const void *a, const void *b) {
        return strcmp(((const IndexEntry *)a)->path,
                      ((const IndexEntry *)b)->path);
    }

    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), cmp_entries);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(f, "%o %s %llu %u %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long long)sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}

// ✅ FIXED: main crash solution
int index_add(Index *index, const char *path) {

    // 🔥 CRITICAL FIX: handle uninitialized index
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES) {
        index->count = 0;
    }

    // Always load existing index safely
    if (index_load(index) != 0) {
        fprintf(stderr, "error: failed to load index\n");
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    void *contents = NULL;

    // ✅ FIX: avoid malloc(0)
    if (file_size > 0) {
        contents = malloc(file_size);
        if (!contents) {
            fclose(f);
            return -1;
        }

        size_t nread = fread(contents, 1, file_size, f);
        if (nread != (size_t)file_size) {
            free(contents);
            fclose(f);
            return -1;
        }
    }

    fclose(f);

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, file_size, &blob_id) != 0) {
        free(contents);
        return -1;
    }

    free(contents);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *entry = index_find(index, path);

    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;

        entry = &index->entries[index->count++];
        memset(entry, 0, sizeof(IndexEntry));  // ✅ important
        snprintf(entry->path, sizeof(entry->path), "%s", path);
    }

    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    return index_save(index);
}