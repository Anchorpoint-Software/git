#ifndef VIRTUAL_FS_H
#define VIRTUAL_FS_H

struct object_id;

void init_anchorpoint_mutex(void);
int init_anchorpoint_process(void);

int is_path_virtual(const char* path);
int create_placeholder(const char *path, unsigned int size, const struct object_id *oid);
int convert_to_placeholder(const char *path, const struct object_id *oid);
int is_sync_root(const char *path);
int set_sync_state(const char *path, int in_sync);
int get_placeholder_identifier(const char *path, struct object_id *oid);

#endif
