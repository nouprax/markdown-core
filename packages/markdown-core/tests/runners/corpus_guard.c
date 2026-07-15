/* Benchmark/test corpus preflight guard.
 *
 * Fails when the workspace contains unmanaged corpus state: a leftover
 * Pro Git checkout, loose generated benchmark input, or a vendored corpus
 * under packages/markdown-core/tests/corpora/ that lacks its manifest,
 * license, or checksum file.
 * Content hashes are additionally verified by scripts/audit-test-topology.sh;
 * this guard keeps the invariant enforced from every test/bench run without
 * needing a shell.
 *
 *   corpus_guard --root REPO_ROOT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static int path_exists(const char *path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return stat(path, &info) == 0;
#endif
}

static int path_is_directory(const char *path) {
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

static int check_absent(const char *root, const char *relative, size_t *failures) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (path_exists(path)) {
        fprintf(stderr, "unmanaged corpus state present: %s\n", relative);
        (*failures)++;
        return -1;
    }
    return 0;
}

static void check_corpus_entry(const char *root, const char *name, size_t *failures) {
    static const char *const REQUIRED[] = {"MANIFEST.json", "LICENSE", "SHA256SUMS"};
    size_t i;
    for (i = 0; i < sizeof(REQUIRED) / sizeof(REQUIRED[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/packages/markdown-core/tests/corpora/%s/%s", root, name, REQUIRED[i]);
        if (!path_exists(path)) {
            fprintf(stderr, "vendored corpus %s is missing %s\n", name, REQUIRED[i]);
            (*failures)++;
        }
    }
}

static void check_corpora(const char *root, size_t *failures) {
    char corpora[1024];
    snprintf(corpora, sizeof(corpora), "%s/packages/markdown-core/tests/corpora", root);
    if (!path_is_directory(corpora))
        return;
#if defined(_WIN32)
    {
        char pattern[1024];
        WIN32_FIND_DATAA entry;
        HANDLE handle;
        snprintf(pattern, sizeof(pattern), "%s\\*", corpora);
        handle = FindFirstFileA(pattern, &entry);
        if (handle == INVALID_HANDLE_VALUE)
            return;
        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(entry.cFileName, ".") != 0 &&
                strcmp(entry.cFileName, "..") != 0)
                check_corpus_entry(root, entry.cFileName, failures);
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
    }
#else
    {
        DIR *directory = opendir(corpora);
        struct dirent *entry;
        if (!directory)
            return;
        while ((entry = readdir(directory)) != NULL) {
            char *path;
            size_t path_size;
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            path_size = strlen(corpora) + 1 + strlen(entry->d_name) + 1;
            path = malloc(path_size);
            if (!path) {
                fputs("unable to allocate corpus path\n", stderr);
                (*failures)++;
                break;
            }
            snprintf(path, path_size, "%s/%s", corpora, entry->d_name);
            if (path_is_directory(path))
                check_corpus_entry(root, entry->d_name, failures);
            free(path);
        }
        closedir(directory);
    }
#endif
}

int main(int argc, char **argv) {
    const char *root = NULL;
    size_t failures = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else {
            fputs("usage: corpus_guard --root REPO_ROOT\n", stderr);
            return 2;
        }
    }
    if (!root) {
        fputs("usage: corpus_guard --root REPO_ROOT\n", stderr);
        return 2;
    }

    check_absent(root, "progit", &failures);
    check_absent(root, "alltests.md", &failures);
    check_absent(root, "packages/markdown-core/benchmarks/benchinput.md", &failures);
    check_absent(root, "packages/markdown-core/tests/core/afl_results", &failures);
    check_corpora(root, &failures);

    if (failures) {
        fprintf(stderr, "%zu corpus policy violation(s)\n", failures);
        return 1;
    }
    printf("corpus preflight guard passed\n");
    return 0;
}
