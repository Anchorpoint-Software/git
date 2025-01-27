#ifndef VIRTUAL_FS_H
#define VIRTUAL_FS_H

struct object_id;

int is_path_virtual(const char* path);
int create_placeholder(const char *path, unsigned int size, const struct object_id *oid);

#endif