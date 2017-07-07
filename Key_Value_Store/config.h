#include <stdlib.h>

#define DATA_BASE_NAME   "database"
#define KEY_MAX_LENGTH   32
#define VALUE_MAX_LENGTH 256

extern int  kv_store_create(const char *name);
extern int  kv_store_write(const char *key, const char *value);
extern char *kv_store_read(const char *key);
extern char **kv_store_read_all(const char *key);
extern int  kv_delete_db();

