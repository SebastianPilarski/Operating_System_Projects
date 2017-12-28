/*
 * This file contains a simple key-value-store API
 *
 * It allows for the creation of a key-value store, writing a key-value pair,
 * reading of a key's values one by one, and reading all-values for a key.
 *
 * The file is organized as follows:
 * 1) Basic structures for key-value store defined
 * 2) Miscellaneous functions including hashing function
 * 3) Initialization functions
 * 4) Write functions
 * 5) Read functions
 * 6) Debug functions
 * 7) API functions
 *
 */

#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <stdlib.h>
#include "config.h"

#define ENTRIES_IN_POD 257
#define PODS_IN_STORE  257

//************************************************************************************
// Structs
//************************************************************************************

struct s_entry {
    char key[KEY_MAX_LENGTH + 1];
    char val[VALUE_MAX_LENGTH + 1];
};

struct s_pod {
    struct s_entry entry[ENTRIES_IN_POD];
    int begin;
    int end;
};

struct s_store {
    struct s_pod pod[PODS_IN_STORE];
};

int last_read_pod[PODS_IN_STORE]; // Keeps track of the last read entry in each pod
struct s_store store;
struct s_store* mm_store;

sem_t* sem[PODS_IN_STORE]; //Semaphore for each pod
char*  db_name;

//************************************************************************************
// Miscellaneous Functions
//************************************************************************************

unsigned hash(const char* str) {
    //Adapted from Dan Bernstein in comp.lang.c
    unsigned long h = 5381;
    int c;

    while(c = *str++) h = ((h << 5) + h) + c; // Hash * 33 + c;
    return (unsigned) h;
}


int inc_pod_index(int i) {
    return (i+1)%ENTRIES_IN_POD;
}

int my_sem_wait(int podID) {
    int status = sem_wait(sem[podID]);
    if(status == -1) printf("Sem_wait failed - pod: %d\n", podID);
    return status;
}

int my_sem_post(int podID) {
    int status = sem_post(sem[podID]);
    if(status == -1) printf("Sem_post failed - pod: %d\n", podID);
    return status;
}


//************************************************************************************
// Init Functions
//************************************************************************************
void init_entry(struct s_entry* e) {
    for(size_t i = 0; i < sizeof(e->key); i++) e->key[i] = 0;
    for(size_t i = 0; i < sizeof(e->val); i++) e->val[i] = 0;
}

void init_pod(struct s_pod* p) {
    for(int i = 0; i < ENTRIES_IN_POD; i++) init_entry(&p->entry[i]);
    p->begin = 0;
    p->end   = 0;
}

void init_store(struct s_store* s) {
    for(int i = 0; i < PODS_IN_STORE; i++) init_pod(&s->pod[i]);
}

void init_sem(void) {
    char semNames[50] = "";
    for(int i = 0; i < PODS_IN_STORE; i++) {
        sprintf(semNames, "mySemaphore_%d", i);
        sem[i] = sem_open(semNames, O_CREAT, S_IRWXU, 1);
        if(sem[i] == SEM_FAILED) {
            printf("Creating semaphore failed\n");
            for(int j = 0; j < i; j++) {
                sprintf(semNames, "mySemaphore_%d", j);
                sem_unlink(semNames);
                sem_close(sem[j]);
            }
            exit(1);
        }
    }
}

void close_sem(void) {
    char semNames[50] = "";
    for(int i = 0; i < PODS_IN_STORE; i++) {
        sprintf(semNames, "mySemaphore_%d", i);
        sem_unlink(semNames);
        sem_close(sem[i]);
    }
}

//************************************************************************************
// Write Functions
//************************************************************************************

void write_entry(struct s_entry* s, const char* key, const char* val) {
    strncpy(&s->key[0], key, KEY_MAX_LENGTH);
    strncpy(&s->val[0], val, VALUE_MAX_LENGTH);
}

int write_pod(struct s_pod* p, const char* key, const char* val) {
    int found = 0;
    for(int i = p->begin; i != p->end; i = inc_pod_index(i)) {
        if(!strncmp(key, p->entry[i].key, KEY_MAX_LENGTH) &&
           !strncmp(val, p->entry[i].val, VALUE_MAX_LENGTH)) {
            found = 1;
            break;
        }
    }

    if(!found) {
        write_entry(&p->entry[p->end], key, val);
        p->end = inc_pod_index(p->end);

        if(p->begin == p->end) p->begin = inc_pod_index(p->begin);
    }
    return found;
}

int write_store(struct s_store* s, const char* key, const char* val) {
    if(key == NULL || val == NULL) return 1;
    int podID = hash(key) % PODS_IN_STORE;
    if(my_sem_wait(podID) == -1) return 1;
    int res = write_pod(&s->pod[podID], key, val);
    my_sem_post(podID);
    return res;
}

//************************************************************************************
// Read Functions
//************************************************************************************

char* read_entry(struct s_entry* s) {
    char* c = calloc(VALUE_MAX_LENGTH+1, sizeof(char));
    strncpy(c, s->val, VALUE_MAX_LENGTH);
    return c;
}

char* read_pod(struct s_pod* p, const char* key, const int podID) {
    if(p->begin == p->end) return NULL; // Return if pod empty

    int current = last_read_pod[podID];

    for(int i = 0; i < ENTRIES_IN_POD; i++) {
        if(current == p->end) current = p->begin;
        if(p->entry[current].key == NULL) break;
        if(!strncmp(p->entry[current].key, key, KEY_MAX_LENGTH)) {
            char* val            = read_entry(&p->entry[current]);
            current              = inc_pod_index(current);
            last_read_pod[podID] = current;
            return val;
        }
        current = inc_pod_index(current);
    }
    return NULL;                                      // None found
}

char* read_store(struct s_store* s, const char* key) {
    if(key == NULL) return NULL;
    int podID = hash(key) % PODS_IN_STORE;
    if(my_sem_wait(podID) == -1) return NULL;
    char* val = read_pod(&s->pod[podID], key, podID);
    my_sem_post(podID);
    return val;
}

char** read_pod_all(struct s_pod* p, const char* key) {
    char** c = calloc(ENTRIES_IN_POD+1, sizeof(char*));
    int found = 0;
    for(int i = p->begin; i != p->end; i = inc_pod_index(i)) {
        if(!strncmp(key, p->entry[i].key, KEY_MAX_LENGTH)) {
            c[found++] = read_entry(&p->entry[i]);
        }
    }
    return c;
}

char** read_store_all(struct s_store* s, const char* key) {
    if(key == NULL) return NULL;
    int podID = hash(key) % PODS_IN_STORE;

    if(my_sem_wait(podID) == -1) return NULL;
    char** c = read_pod_all(&s->pod[podID], key);
    my_sem_post(podID);
    return c;
}

//************************************************************************
// Debug functions
//************************************************************************
void printf_entry(const struct s_entry* e) {
    printf("%s\t%s\n", e->key, e->val);
}

void printf_pod(const struct s_pod* p) {
    for(int i = 0; i < PODS_IN_STORE; i++) {
        printf_entry(&p->entry[i]);
    }
    printf("\n");
}

//***********************************************************************
// Key-Value Store API
//***********************************************************************

sem_t* sem_clr;
int kv_store_create(const char* name) {
    init_sem();

    int fd = shm_open(name, O_CREAT|O_RDWR, S_IRWXU);
    if(fd < 0) {
        printf("Failed to create shared memory object\n");
        return 1;
    }

    char* addr = mmap(NULL, sizeof(struct s_store), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    ftruncate(fd, sizeof(struct s_store));
    close(fd);
    mm_store = (struct s_store*) addr;

    sem_clr = sem_open("sem_unique", O_CREAT | O_EXCL, S_IRWXU, 1);
    if(sem_clr != SEM_FAILED) {
        init_store(mm_store);
    }

    db_name = calloc(strlen(name)+1, sizeof(char));
    strcpy(db_name, name);
    return 0;
}

int kv_store_write(const char* key, const char* value) {
    return write_store(mm_store, key, value); //note: returns 0 on success, 1 on failure
}

char* kv_store_read(const char* key) {
    return read_store(mm_store, key);
}

char** kv_store_read_all(const char* key) {
    char** c = read_store_all(mm_store, key);
    if(c[0] == NULL) { //To satisfy automatic test
        free(c);
        c = NULL;
    }
    return c;
}

int kv_delete_db() {
    sem_unlink("sem_unique");
    sem_close(sem_clr);
    close_sem();
    munmap(mm_store, sizeof(struct s_store));

    int fd = shm_unlink(db_name);
    if(fd < 0) {
        printf("No shared memory linked\n");
        return 0;
    }
    else printf("Shared memory unlinked\n");

    free(db_name);
    db_name = NULL;
    return 0;
}