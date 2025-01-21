#include "virtual_fs.h"
#include "cfapi.h"
//#include "vcruntime.h"
#include <stdbool.h>
#include <vcruntime.h>

int create_virtual_placeholder(const char *path, const char *target) {
    (void)path;
    (void)target;

    // CF_PLACEHOLDER_CREATE_INFO cloud_entry;
    // CfCreatePlaceholders(NULL, &cloud_entry, 1, CF_CREATE_FLAG_NONE, NULL);

    return 0;
}