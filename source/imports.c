#include <string.h>

#include "imports.h"

static const struct {
    const import_entry *table;
    const int *count;
} tables[] = {
    {patch_imports, &patch_imports_count},
    {darwin_imports, &darwin_imports_count},
    {gles_imports, &gles_imports_count},
    {coreaudio_imports, &coreaudio_imports_count},
    {objc_imports, &objc_imports_count},
};

const import_entry *imports_lookup(const char *name) {
    for (unsigned t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        if (!tables[t].table || !tables[t].count)
            continue;
        for (int i = 0; i < *tables[t].count; i++) {
            if (!strcmp(tables[t].table[i].name, name))
                return &tables[t].table[i];
        }
    }
    return NULL;
}
