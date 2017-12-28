/*
 * This file contains the Shadow File System API
 *
 * The shadows are implemented as FIFO. The filesystem stores four shadows. Restoring number 1
 * will restore the most recent commit.
 *
 * This file is organized as follows:
 * 1) Structures for filesystem defined
 * 2) Functions to initialize these structures
 * 3) Miscellaneous functions for converting indexes
 * 4) Functions for synchronizing disk with file data structures
 * 5) File manipulation functions [Helpers] - they are grouped according to functionalities
 * 6) System api functions are on bottom of file
 * 7) On bottom of file there is my commented-out main function which runs and prints basic worksings
 *
 */

// Disk Filesystem Structure
//*****************************************************************************************************************
// Super | I_NODE File   |         Data Blocks          |  Shadow Dir N   |     Dir 0     |   FBM     |    WM     *
//   0   |   1 to 13     |   14 to #BLOCKS-2-(N+1)-1    | #BLOCKS-2-(N+1) | #BLOCKS-2-(1) | #BLOCKS-2 | #BLOCKS-1 *
//*****************************************************************************************************************
// Block Content
// Block Number

#include "sfs_api.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "disk_emu.h"

#define MAGIC_NUMBER          0xACBD0005
#define NUMBER_OF_BYTES_BLOCK 1024
#define NUMBER_OF_BLOCKS      1024
#define NUMBER_OF_POINTERS    14
#define NODE_SIZE             ((NUMBER_OF_POINTERS+2)*4)
#define NUMBER_OF_I_NODES     200
#define MAX_NODE_IN_BLOCK     ((NUMBER_OF_BYTES_BLOCK / NODE_SIZE))
#define BLOCKS_I_NODE_FILE    ((NUMBER_OF_I_NODES / MAX_NODE_IN_BLOCK) + (NUMBER_OF_I_NODES % MAX_NODE_IN_BLOCK ? 1 : 0))
#define NUMBER_OF_J_NODES     ((NUMBER_OF_BYTES_BLOCK-(4*4))/sizeof(struct s_node))
#define MAX_NAME_LENGTH       20
#define MAX_FILES             (NUMBER_OF_BYTES_BLOCK/sizeof(struct s_dir_entry))
#define MAX_DIRS_INCL_SHAD    5
#define MAX_FD                32
#define FIRST_DATA_BLOCK      (1+BLOCKS_I_NODE_FILE)
#define LAST_DATA_BLOCK       (NUMBER_OF_BLOCKS-1-2-MAX_DIRS_INCL_SHAD)
#define POINTERS_IND_BLOCK    (NUMBER_OF_BYTES_BLOCK/sizeof(ptr_t))


typedef uint32_t ptr_t;
typedef uint32_t ind_ptr_t;

//********************************************************************************
// File System Structures
//********************************************************************************

struct s_node {
    int32_t   size;
    ptr_t     pointer[NUMBER_OF_POINTERS];
    ind_ptr_t ind_pointer;
};

struct s_super_block {
    union {
        struct {
            uint32_t        magic;
            uint32_t        block_size;
            uint32_t        num_blocks;
            uint32_t        num_i_nodes;
            struct s_node   j_node[NUMBER_OF_J_NODES];
        };
        uint8_t block_space[NUMBER_OF_BYTES_BLOCK];
    };
};

struct s_node_block {
    union {
        struct s_node i_node[MAX_NODE_IN_BLOCK];
        uint8_t block_space[NUMBER_OF_BYTES_BLOCK];
    };
};

struct s_node_file {
    struct s_node_block block[BLOCKS_I_NODE_FILE];
};

struct s_bit_map {
    union {
        uint8_t block_group[NUMBER_OF_BLOCKS / 8 + (NUMBER_OF_BLOCKS % 8 ? 1 : 0)];
        uint8_t block_space[NUMBER_OF_BYTES_BLOCK];
    };
};

struct s_ind_node_block {
    ptr_t pointer[NUMBER_OF_BYTES_BLOCK/sizeof(ptr_t)];
};

struct s_dir_entry {
    char     name[MAX_NAME_LENGTH+1];
    uint32_t i_node_number;
};

struct s_dir {
    union {
        struct s_dir_entry entry[MAX_FILES];
        uint8_t            block_space[NUMBER_OF_BYTES_BLOCK];
    };
};

struct s_file_system {
    struct s_super_block super_block;
    struct s_node_file   i_node_file;
    struct s_dir         directory[MAX_DIRS_INCL_SHAD];
    struct s_bit_map     free_bit_map;
    struct s_bit_map     write_mask;
};

struct s_data_block {
    char c[NUMBER_OF_BYTES_BLOCK];
};

struct s_file_pointer {
    ptr_t block;
    ptr_t c_ptr;
};

struct s_fd {
    struct s_dir_entry    entry;
    struct s_file_pointer read_pointer;
    struct s_file_pointer write_pointer;
};

struct s_open_file_table {
    struct s_fd file[MAX_FD];
};

struct s_open_file_table open_file_table;
struct s_file_system     file_system;

//***********************************************************************************
// BitMap Related Functions
//***********************************************************************************

void set_bit(uint8_t* block_group, int index) {
    *block_group |= 1u << index;
}

void clr_bit(uint8_t* block_group, int index) {
    *block_group &= ~(1u << index);
}

int get_bit(uint8_t* block_group, int index) {
    return (*block_group >> index) & 1u;
}

void set_bit_map(struct s_bit_map* map, int block) {
    set_bit(&map->block_group[block/8], block%8);
}

void clr_bit_map(struct s_bit_map* map, int block) {
    clr_bit(&map->block_group[block/8], block%8);
}

int get_bit_map(struct s_bit_map* map, int block) {
    return get_bit(&map->block_group[block/8], block%8);
}

//***********************************************************************************
// Init Functions
//***********************************************************************************

void init_node(struct s_node* node) {
    node->size        = -1;
    node->ind_pointer = 0;
    for(int i = 0; i < NUMBER_OF_POINTERS; i++) node->pointer[i] = 0;
}

void init_node_block(struct s_node_block* node_block) {
    for(int i = 0; i < MAX_NODE_IN_BLOCK; i++) init_node(&node_block->i_node[i]);
}

void init_node_file(struct s_node_file* node_file) {
    for(int i = 0; i < BLOCKS_I_NODE_FILE; i++) init_node_block(&node_file->block[i]);
}

void init_super_block(struct s_super_block* super_block) {
    super_block->magic          = MAGIC_NUMBER;
    super_block->num_blocks     = NUMBER_OF_BLOCKS;
    super_block->num_i_nodes    = NUMBER_OF_I_NODES;
    for(int i = 0; i < NUMBER_OF_J_NODES; i++) init_node(&super_block->j_node[i]);

    // Initializing root j nodes and pointers to i_node file
    for(int i = 0; i < MAX_DIRS_INCL_SHAD; i++) {
        super_block->j_node[i].size = sizeof(struct s_node) * NUMBER_OF_I_NODES;
        for(ptr_t j = 0; j < BLOCKS_I_NODE_FILE; j++) super_block->j_node[i].pointer[j] = j + 1;
    }
}

void init_map(struct s_bit_map* bit_map) {
    for(size_t i = 0; i < sizeof(bit_map->block_group); i++) bit_map->block_group[i] = 0xff;
}

void init_ind_node_block(struct s_ind_node_block* ind_node_block) {
    for(int i = 0; i < POINTERS_IND_BLOCK; i++) ind_node_block->pointer[i] = 0;
}

void init_data_block(struct s_data_block* data_block) {
    for(int i = 0; i < NUMBER_OF_BYTES_BLOCK; i++) {
        data_block->c[i] = '\0';
    }
}

void init_dir_entry(struct s_dir_entry* dir_entry) {
    for(int i = 0; i <= MAX_NAME_LENGTH; i++) dir_entry->name[i] = '\0'; //clear name
    dir_entry->i_node_number = 0;
}

void init_dir(struct s_dir* dir) {
    for(int i = 0; i < MAX_FILES; i++) init_dir_entry(&dir->entry[i]);
}

void init_file_system(struct s_file_system* file_system) {
    init_super_block(&file_system->super_block);
    init_node_file(&file_system->i_node_file);
    for(int i = 0; i < MAX_DIRS_INCL_SHAD; i++) init_dir(&file_system->directory[i]);
    init_map(&file_system->free_bit_map);
    init_map(&file_system->write_mask);

    for(int i = 0; i < MAX_DIRS_INCL_SHAD; i++) {
        file_system->i_node_file.block[0].i_node[i].size       = 0;
        file_system->i_node_file.block[0].i_node[i].pointer[0] = NUMBER_OF_BLOCKS-2-(i+1);
    }

    for(int i = 0; i <= BLOCKS_I_NODE_FILE; i++) {
        clr_bit_map(&file_system->free_bit_map, i);
        clr_bit_map(&file_system->write_mask, i);
    }
    for(int i = NUMBER_OF_BLOCKS-1; i >= (NUMBER_OF_BLOCKS-2-(MAX_DIRS_INCL_SHAD+1)); i--) {
        clr_bit_map(&file_system->free_bit_map, i);
        clr_bit_map(&file_system->write_mask, i);
    }
}

void init_file_pointer(struct s_file_pointer* file_pointer) {
    file_pointer->block = 0;
    file_pointer->c_ptr = 0;
}

void init_fd(struct s_fd* fd) {
    init_dir_entry(&fd->entry);
    init_file_pointer(&fd->read_pointer);
    init_file_pointer(&fd->write_pointer);
}

void init_open_file_table(struct s_open_file_table* table) {
    for(int i = 0; i < MAX_FD; i++) init_fd(&table->file[i]);
}

//*********************************************************************************
// Miscellaneous functions
//*********************************************************************************

int node_number_to_block(uint32_t i_node_number) {
    return i_node_number/MAX_NODE_IN_BLOCK;
}

int node_number_to_node_in_block(uint32_t i_node_number) {
    return i_node_number%MAX_NODE_IN_BLOCK;
}

//*********************************************************************************
// Functions for disk synchronization
//*********************************************************************************


void dump_file_system_to_disk(void)
{
    write_blocks(0, 1, &file_system.super_block);
    write_blocks(NUMBER_OF_BLOCKS-1, 1, &file_system.write_mask);
    write_blocks(NUMBER_OF_BLOCKS-2, 1, &file_system.free_bit_map);
    write_blocks(1, BLOCKS_I_NODE_FILE, &file_system.i_node_file);
    for(int i = 0; i < MAX_DIRS_INCL_SHAD; i++) write_blocks(NUMBER_OF_BLOCKS-2-(i+1), 1, &file_system.directory[i]);
}

void load_file_system_from_disk(void)
{
    read_blocks(0, 1, &file_system.super_block);
    read_blocks(NUMBER_OF_BLOCKS-1, 1, &file_system.write_mask);
    read_blocks(NUMBER_OF_BLOCKS-2, 1, &file_system.free_bit_map);
    read_blocks(1, BLOCKS_I_NODE_FILE, &file_system.i_node_file);
    for(int i = 0; i < MAX_DIRS_INCL_SHAD; i++) read_blocks(NUMBER_OF_BLOCKS-2-(i+1), 1, &file_system.directory[i]);
}

//*********************************************************************************
// File Manipulation functions -- fopen
//*********************************************************************************

int get_last_file_block(int i_node_number);
int get_end_char(int i_node_number);


int get_free_block(struct s_file_system* file_system) {
    for(int i = FIRST_DATA_BLOCK; i <= LAST_DATA_BLOCK; i++) {
        if(get_bit_map(&file_system->free_bit_map, i)) {
            clr_bit_map(&file_system->free_bit_map, i);
            return i;
        }
    }
    printf("No free blocks\n");
    return -1;
}

int get_free_i_node(struct s_file_system* file_system, int* i_block) {
    for(int i = 0; i < BLOCKS_I_NODE_FILE; i++) {
        for(int j = 0; j < MAX_NODE_IN_BLOCK; j++) {
            if(file_system->i_node_file.block[i].i_node[j].pointer[0] == 0) {
                *i_block = i;
                return j;
            }
        }
    }
    printf("No free i nodes\n");
    return -1;
}

int add_file_to_dir(struct s_file_system* file_system, char* name, int* i_block, int* i_node) {
    int i = 0;
    for(i = 0; i < MAX_FILES; i++) {
        if(file_system->directory[0].entry[i].name[0] == '\0') break;
    }

    if(i >= MAX_FILES) {
        printf("Directory is full\n");
        return -1;
    }

    *i_block = -1;
    *i_node  = get_free_i_node(file_system, i_block);
    if(*i_node == -1) return -1;

    int block = get_free_block(file_system);
    if(block == -1) return -1;

    file_system->i_node_file.block[*i_block].i_node[*i_node].size = 0;
    strncpy(file_system->directory[0].entry[i].name, name, MAX_NAME_LENGTH);
    file_system->directory[0].entry[i].i_node_number = (*i_node) + (*i_block)*MAX_NODE_IN_BLOCK;
    file_system->i_node_file.block[*i_block].i_node[*i_node].pointer[0] = block;

    write_blocks(NUMBER_OF_BLOCKS-2-(1), 1, &file_system->directory[0]);
    write_blocks(1 + *i_block, 1, &file_system->i_node_file.block[*i_block]);
    return i; // Returns directory index
}

void set_fopen_name(struct s_file_system* file_system, char* name, int index_table) {
    strncpy(open_file_table.file[index_table].entry.name, name, MAX_NAME_LENGTH);
}

void set_read_ptr(int i_node_number, int index_table) {
    int      i_block  = node_number_to_block(i_node_number);
    int      i_node   = node_number_to_node_in_block(i_node_number);
    open_file_table.file[index_table].read_pointer.block = file_system.i_node_file.block[i_block].i_node[i_node].pointer[0];
    open_file_table.file[index_table].read_pointer.c_ptr = 0;
}

int set_write_ptr(int i_node_number, int index_table) {
    open_file_table.file[index_table].write_pointer.block = get_last_file_block(i_node_number);
    open_file_table.file[index_table].write_pointer.c_ptr = get_end_char(i_node_number);

    return 0;
}

int set_fopen_ptrs(struct s_file_system* file_system, int index_sys, int index_table) {
    uint32_t i_node_number = file_system->directory[0].entry[index_sys].i_node_number;

    int err = 0;
    err = set_write_ptr(i_node_number, index_table);
    if(err) return -1; // Because may have to load indirect_block from disk

    open_file_table.file[index_table].entry.i_node_number = i_node_number;
    set_read_ptr(i_node_number, index_table);
    return 0;
}

int create_open_file_entry(struct s_file_system* file_system, char* name, int index) {
    int i = 0;
    for(i = 0; i < MAX_FD; i++) {
        if(open_file_table.file[i].entry.name[0] == '\0') break;
    }

    if(i >= MAX_FD) {
        printf("Cannot open file - file descriptor table is full\n");
        return -1;
    }

    int err = set_fopen_ptrs(file_system, index, i);
    if(err) return -1;

    set_fopen_name(file_system, name, i);
    return i; // Returns open_file_table index
}

int fopen_existing(struct s_file_system* file_system, char* name, int index) {
    for(int i = 0; i < MAX_FD; i++) { //make sure only one of each file open
        if(!strncmp(name, open_file_table.file[i].entry.name, MAX_NAME_LENGTH)) {
            printf("File already open\n");
            return -1;
        }
    }

    return create_open_file_entry(file_system, name, index);
}

int fopen_new(struct s_file_system* file_system, char* name) {
    for(int i = 0; i < MAX_FD; i++) { // Check for previously opened, still not in directory
        if(!strncmp(open_file_table.file[i].entry.name, name, MAX_NAME_LENGTH)) return i;
    }

    int i = 0;
    for(i = 0; i < MAX_FD; i++) if(open_file_table.file[i].entry.name[0] == '\0') break;
    assert(i < MAX_FD);

    int i_block = -1;
    int i_node  = -1;

    int entry = add_file_to_dir(file_system, name, &i_block, &i_node);
    if(entry < 0) return -1;

    strncpy(open_file_table.file[i].entry.name, name, MAX_NAME_LENGTH);
    open_file_table.file[i].entry.i_node_number = file_system->directory[0].entry[entry].i_node_number;
    open_file_table.file[i].read_pointer.block  = file_system->i_node_file.block[i_block].i_node[i_node].pointer[0];
    open_file_table.file[i].read_pointer.c_ptr  = 0;
    open_file_table.file[i].write_pointer.block = file_system->i_node_file.block[i_block].i_node[i_node].pointer[0];
    open_file_table.file[i].write_pointer.c_ptr = 0; // New, nothing written yet, pointing at first char
    return i; // returns index of file_descriptor
}

int check_fd_full() { // Returns -1 if full, 0 if not full
    for(int i = 0; i < MAX_FD; i++) if(open_file_table.file[i].entry.name[0] == '\0') return 0;

    printf("ERROR: Maximum open files\n");
    return -1;
}


//*********************************************************************************
// File_manipulation files f_remove
//*********************************************************************************

void rm_fd(char* file) { // TODO -- potentially write to disk
    for(int i = 0; i < MAX_FD; i++) {
        if(!strncmp(open_file_table.file[i].entry.name, file, MAX_NAME_LENGTH)) {
            init_fd(&open_file_table.file[i]);
        }
    }
}

int rm_file_from_disk(int shadow_number, int entry_index, struct s_file_system* file_system) {
    uint32_t i_node_number = file_system->directory[shadow_number].entry[entry_index].i_node_number;
    int      i_block       = node_number_to_block(i_node_number);
    int      node_in_block = node_number_to_node_in_block(i_node_number);

    for(int i = 0; i < NUMBER_OF_POINTERS; i++) {
        ptr_t rm_block = file_system->i_node_file.block[i_block].i_node[node_in_block].pointer[i];
        if(!rm_block) break;
        set_bit_map(&file_system->free_bit_map, rm_block);
        set_bit_map(&file_system->write_mask, rm_block);
    }

    if(file_system->i_node_file.block[i_block].i_node[node_in_block].ind_pointer) {
        int err = 0;
        struct s_ind_node_block ind_node_block;
        err = read_blocks(file_system->i_node_file.block[i_block].i_node[node_in_block].ind_pointer, 1, &ind_node_block);
        if(err) {
            printf("Error reading indirect block in rm_file_from_disk\n");
        }

        else {
            for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
                ptr_t rm_block = ind_node_block.pointer[i];
                if(!rm_block) break;
                set_bit_map(&file_system->free_bit_map, rm_block);
                set_bit_map(&file_system->write_mask, rm_block);
            }
            ptr_t rm_block = file_system->i_node_file.block[i_block].i_node[node_in_block].ind_pointer;
            set_bit_map(&file_system->free_bit_map, rm_block);
            set_bit_map(&file_system->write_mask, rm_block);
        }

    }

    init_node(&file_system->i_node_file.block[i_block].i_node[node_in_block]);
    return 0;
}

int rm_file_from_dir(char* name, struct s_file_system* file_system) {
    for(int i = 0; i < MAX_FILES; i++) {
        if(!strncmp(file_system->directory[0].entry[i].name, name, MAX_NAME_LENGTH)) {
            rm_file_from_disk(0, i, file_system);
            init_dir_entry(&file_system->directory[0].entry[i]);
            dump_file_system_to_disk();
            return 0;
        }
    }
    printf("Error: File does not exist\n");
    return -1;
}

//*********************************************************************************
// File manipulation - Write
//*********************************************************************************

int add_block(int i_node_number) {
    int block_ptr = get_free_block(&file_system);
    if(block_ptr < 0) return -1;

    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    struct s_node* node = &file_system.i_node_file.block[i_block].i_node[i_node];

    for(int i = 0; i < NUMBER_OF_POINTERS; i++) {
        if(!node->pointer[i]) {
            node->pointer[i] = block_ptr;
            return block_ptr;
        }
    }

    struct s_ind_node_block ind_node_block;
    if(!node->ind_pointer) {
        int ind_block_ptr = get_free_block(&file_system);
        if(ind_block_ptr < 0) {
            set_bit_map(&file_system.free_bit_map, block_ptr);
            set_bit_map(&file_system.write_mask, block_ptr);
            return -1;
        }
        node->ind_pointer = ind_block_ptr;
        init_ind_node_block(&ind_node_block);
        ind_node_block.pointer[0] = block_ptr;
        write_blocks(ind_block_ptr, 1, &ind_node_block);
        return block_ptr;
    }

    read_blocks(node->ind_pointer, 1, &ind_node_block);

    for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
        if(!ind_node_block.pointer[i]) {
            ind_node_block.pointer[i] = block_ptr;
            write_blocks(node->ind_pointer, 1, &ind_node_block);
            return block_ptr;
        }
    }

    printf("Error: Out of block pointers\n");
    return -1;
}

int get_next_file_block(int i_node_number, int block) {
    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    struct s_node* node = &file_system.i_node_file.block[i_block].i_node[i_node];

    for(int i = 0; i < NUMBER_OF_POINTERS; i++) {
        if(node->pointer[i] == block) {
            if(i+1 < NUMBER_OF_POINTERS) return node->pointer[i+1] ? node->pointer[i+1] : -1;
            if(!node->ind_pointer) return -1;

            struct s_ind_node_block ind_node_block;
            read_blocks(node->ind_pointer, 1, &ind_node_block);

            return ind_node_block.pointer[0] ? ind_node_block.pointer[0] : -1;
        }
    }

    struct s_ind_node_block ind_node_block;
    read_blocks(node->ind_pointer, 1, &ind_node_block);

    for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
        if(ind_node_block.pointer[i] == block) {
            if(i+1 < POINTERS_IND_BLOCK) return ind_node_block.pointer[i+1] ? ind_node_block.pointer[i+1] : -1;
            return -1;
        }
    }

    assert(0); // Inconsistency - block not in file
    return -1;
}

int get_last_file_block(int i_node_number) {
    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    struct s_node* node = &file_system.i_node_file.block[i_block].i_node[i_node];

    int last = -1;

    for(int i = 0; i < NUMBER_OF_POINTERS; i++) {
        if (!node->pointer[i]) return last;
        last = node->pointer[i];
    }
    if (!node->ind_pointer) return last;

    struct s_ind_node_block ind_node_block;
    read_blocks(node->ind_pointer, 1, &ind_node_block);

    for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
        if(!ind_node_block.pointer[i]) return last;
        last = ind_node_block.pointer[i];
    }

    return last;
}

// Find the number of blocks in file
int get_num_file_blocks(int i_node_number) {
    int num = 0;
    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    struct s_node* node = &file_system.i_node_file.block[i_block].i_node[i_node];

    for(int i = 0; i < NUMBER_OF_POINTERS; i++) {
        if (!node->pointer[i]) return num;
        num++;
    }

    if (!node->ind_pointer) return num;

    struct s_ind_node_block ind_node_block;
    read_blocks(node->ind_pointer, 1, &ind_node_block);

    for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
        if(!ind_node_block.pointer[i]) return num;
        num++;
    }

    return num;
}

int get_file_size(int i_node_number) {
    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    return file_system.i_node_file.block[i_block].i_node[i_node].size;
}

int get_end_char(int i_node_number) {
    int size = get_file_size(i_node_number);
    int end  = size % NUMBER_OF_BYTES_BLOCK;
    if(!end)
        if(size == get_num_file_blocks(i_node_number) * NUMBER_OF_BYTES_BLOCK )
            end = NUMBER_OF_BYTES_BLOCK;
    return end;
}

void inc_file_size(int i_node_number, int delta) {
    int i_block = node_number_to_block(i_node_number);
    int i_node  = node_number_to_node_in_block(i_node_number);
    file_system.i_node_file.block[i_block].i_node[i_node].size += delta;
}

//*********************************************************************************
// Seek Functions
//*********************************************************************************

int seek_block(int fileID, int loc) {
    if(loc < 0) return -1;
    int block_in_file = loc/NUMBER_OF_BYTES_BLOCK;

    int i_node_number = open_file_table.file[fileID].entry.i_node_number;
    int i_block       = node_number_to_block(i_node_number);
    int i_node        = node_number_to_node_in_block(i_node_number);
    int block         = 0;


    if(block_in_file < NUMBER_OF_POINTERS) {
        block = file_system.i_node_file.block[i_block].i_node[i_node].pointer[block_in_file];
        if(block) return block;
        else           return -1;
    }

    int ind_block_ptr = file_system.i_node_file.block[i_block].i_node[i_node].ind_pointer;
    if(!ind_block_ptr) return -1;

    struct s_ind_node_block ind_node_block;
    read_blocks(ind_block_ptr, 1, &ind_node_block);

    int block_in_ind_block = block_in_file - NUMBER_OF_POINTERS;
    block = ind_node_block.pointer[block_in_ind_block];

    if(block) return block;
    else return -1;
}

int seek_char(int fileID, int loc) {
    if(loc < 0) return -1;
    int char_in_block = loc % NUMBER_OF_BYTES_BLOCK;

    int i_node_number = open_file_table.file[fileID].entry.i_node_number;

    int block = seek_block(fileID, loc);
    if(block == get_last_file_block(i_node_number)) {
        if(char_in_block <= get_end_char(i_node_number)) return char_in_block;
        else return -1;
    }

    return char_in_block;
}

//*********************************************************************************
// Shadowing functions
//*********************************************************************************

void copy_block(int blk_src, int blk_dst) {
    struct s_data_block data_block;
    read_blocks(blk_src, 1, &data_block);
    write_blocks(blk_dst, 1, &data_block);
}

int copy_file(int inn_orig, int inn_copy) {
    int i_block_orig = node_number_to_block(inn_orig);
    int i_node_orig  = node_number_to_node_in_block(inn_orig);
    int i_block_copy = node_number_to_block(inn_copy);
    int i_node_copy  = node_number_to_node_in_block(inn_copy);

    struct s_node* n_orig = &file_system.i_node_file.block[i_block_orig].i_node[i_node_orig];
    struct s_node* n_copy = &file_system.i_node_file.block[i_block_copy].i_node[i_node_copy];

    n_copy->size = n_orig->size;

    copy_block(n_orig->pointer[0], n_copy->pointer[0]);

    for(int i = 1; i < NUMBER_OF_POINTERS; i++) {
        if(!n_orig->pointer[i]) return 0;
        int blk = get_free_block(&file_system);
        if(blk < 0) return -1;
        copy_block(n_orig->pointer[i], n_copy->pointer[i]);
    }

    if(!n_orig->ind_pointer) return 0;

    struct s_ind_node_block ind_node_block_orig;
    read_blocks(n_orig->ind_pointer, 1, &ind_node_block_orig);

    int blk = get_free_block(&file_system);
    if(blk < 0) return -1;
    n_copy->ind_pointer = blk;

    struct s_ind_node_block ind_node_block_copy;
    init_ind_node_block(&ind_node_block_copy);

    for(int i = 0; i < POINTERS_IND_BLOCK; i++) {
        if(!ind_node_block_orig.pointer[i]) return 0;
        blk = get_free_block(&file_system);
        if(blk < 0) return -1;
        copy_block(ind_node_block_orig.pointer[i], ind_node_block_copy.pointer[i]);
    }

    return 0;
}

void free_shadow_directory(int shadow)
{
    for(int i = 0; i < MAX_FILES; i++) {
        if(file_system.directory[shadow].entry[i].name[0] != '\0') {
            rm_file_from_disk(shadow, i, &file_system);
            init_dir_entry(&file_system.directory[shadow].entry[i]);
        }
    }
    dump_file_system_to_disk();
}

int  restore_shadow_directory(int shadow)
{
    if(shadow <= 0 || shadow >= MAX_DIRS_INCL_SHAD) {
        printf("Error, please choose shadow 1 through %d\n", MAX_DIRS_INCL_SHAD-1);
        return -1;
    }

    struct s_data_block data_block;
    for(int i = 0; i < MAX_FILES; i++) {
        if(file_system.directory[shadow].entry[i].name[0] != '\0') {
            char* name = file_system.directory[shadow].entry[i].name;
            int i_block_copy = 0;
            int i_node_copy  = 0;
            int e = add_file_to_dir(&file_system, name, &i_block_copy, &i_node_copy);
            if(e < 0) {
                FAILED: rm_file_from_dir(name,&file_system);
                return -1;
            }
            int inn_orig = file_system.directory[shadow].entry[i].i_node_number;
            int inn_copy = file_system.directory[0     ].entry[e].i_node_number;
            int err      = copy_file(inn_orig, inn_copy);
            if(err < 0) {
                printf("Not enough disk space to restore full directory\n");
                goto FAILED;
            }
        }
    }
    dump_file_system_to_disk();
}

//*********************************************************************************
// Test Functions and Debugging
//*********************************************************************************

void print_directory(int shadow) {
    for(int i = 0; i < MAX_FILES; i++) {
        if(file_system.directory[shadow].entry[i].name[0] != '\0') {
            printf("\nFile name: %s\n", file_system.directory[shadow].entry[i].name);
            int i_node_number   = file_system.directory[shadow].entry[i].i_node_number;
            printf("   Inode: %u\n", file_system.directory[shadow].entry[i].i_node_number);
            int i_node_block    = node_number_to_block(i_node_number);
            int i_node_in_block = node_number_to_node_in_block(i_node_number);
            printf("   Size: %d\n", file_system.i_node_file.block[i_node_block].i_node[i_node_in_block].size);
            fflush(stdout);
            for(int j = 0; j < NUMBER_OF_POINTERS; j++) {
                int block_ptr = file_system.i_node_file.block[i_node_block].i_node[i_node_in_block].pointer[j];
                printf("    Block: %u", block_ptr);
                printf(" free: %d   write: %d\n", get_bit_map(&file_system.free_bit_map, block_ptr), get_bit_map(&file_system.write_mask, block_ptr));
            }
        }
    }
}

//**********************************************************************************
// Simple Shadow File System API
//**********************************************************************************

void mkssfs(int fresh) {
    char disk_name[7] = "MyDisk";

    if(fresh) {
        //close_disk(); // TODO -- Causes crash due to bug in external code!!! I submitted bug report + suggested fix
        int err = init_fresh_disk(disk_name, NUMBER_OF_BYTES_BLOCK, NUMBER_OF_BLOCKS);
        if(err) return;

        init_file_system(&file_system);
        dump_file_system_to_disk();

    }
    else {
        int err = init_disk(disk_name, NUMBER_OF_BYTES_BLOCK, NUMBER_OF_BLOCKS);
        load_file_system_from_disk();
        if(err) return;
    }
    init_open_file_table(&open_file_table);
}

int ssfs_fopen(char *name) {
    if(check_fd_full()) return -1;

    if(name == NULL || strlen(name) == 0) {
        printf("ERROR: NO NAME GIVEN\n");
        return -1;
    }

    for(int i = 0; i < MAX_FILES; i++) {
        if(!strncmp(file_system.directory[0].entry[i].name, name, MAX_NAME_LENGTH)) {
            return fopen_existing(&file_system, name, i);
        }
    }

    return fopen_new(&file_system, name);
}

int ssfs_fclose(int fileID) {
    if(fileID < 0 || fileID >= MAX_FD) {
        printf("Error, not a valid fileID, please select between 0 and %d\n", MAX_FD-1);
        return -1;
    }

    if(open_file_table.file[fileID].entry.name[0] == '\0') return -1;
    write_blocks(0, 1, &file_system.super_block);
    write_blocks(1, BLOCKS_I_NODE_FILE, &file_system.i_node_file);
    write_blocks(NUMBER_OF_BLOCKS-2-(1), 1, &file_system.directory[0]);
    write_blocks(NUMBER_OF_BLOCKS-2, 1, &file_system.free_bit_map);
    write_blocks(NUMBER_OF_BLOCKS-1, 1, &file_system.write_mask);
    init_fd(&open_file_table.file[fileID]);
    return 0;
}

int ssfs_frseek(int fileID, int loc) {
    int block         = seek_block(fileID, loc);
    int char_in_block = seek_char(fileID, loc);
    if(block < 0 || char_in_block < 0) {
        printf("Error, read location does not exist\n");
        return -1;
    }

    open_file_table.file[fileID].read_pointer.block = block;
    open_file_table.file[fileID].read_pointer.c_ptr  = char_in_block;
    return 0;
}

int ssfs_fwseek(int fileID, int loc) {
    int block         = seek_block(fileID, loc);
    int char_in_block = seek_char(fileID, loc);
    if(block < 0 || char_in_block < 0) {
        printf("Error, write location does not exist\n");
        return -1;
    }

    open_file_table.file[fileID].write_pointer.block = block;
    open_file_table.file[fileID].write_pointer.c_ptr  = char_in_block;
    return 0;
}

int ssfs_fwrite(int fileID, char* buf, int length) {
    if(buf == NULL || !length) return 0;
    struct s_data_block data_block;
    int lb = get_last_file_block(open_file_table.file[fileID].entry.i_node_number);
    int lc = get_end_char(open_file_table.file[fileID].entry.i_node_number);
    int nb = 0;

    int buf_pos = 0;
    int cb = open_file_table.file[fileID].write_pointer.block;
    int cc = open_file_table.file[fileID].write_pointer.c_ptr;
    int tb = 0;

    FILL_BLOCK:
    // End of block?
    if(cc >= NUMBER_OF_BYTES_BLOCK) {
        tb = get_next_file_block(open_file_table.file[fileID].entry.i_node_number, cb);
        if(tb < 0) nb = add_block(open_file_table.file[fileID].entry.i_node_number);
        if(nb < 0) goto EXIT;
        cc = 0;
        cb = nb ? nb : tb;
    }

    read_blocks(cb, 1, &data_block);

    // Copy buf to current data block
    while(cc < NUMBER_OF_BYTES_BLOCK && buf_pos < length) {
        data_block.c[cc++] = buf[buf_pos++];
        if(nb || cb == lb && cc > lc) inc_file_size(open_file_table.file[fileID].entry.i_node_number, 1);
    }

    write_blocks(cb, 1, &data_block);
    if(buf_pos < length) goto FILL_BLOCK;

    EXIT:
    write_blocks(1, BLOCKS_I_NODE_FILE, &file_system.i_node_file);
    open_file_table.file[fileID].write_pointer.block = cb;
    open_file_table.file[fileID].write_pointer.c_ptr = cc;
    return buf_pos;
}

int ssfs_fread(int fileID, char* buf, int length) {
    if(open_file_table.file[fileID].entry.name[0] == '\0') return -1;
    if(buf == NULL || !length) return 0;
    struct s_data_block data_block;
    int lb = get_last_file_block(open_file_table.file[fileID].entry.i_node_number);
    int lc = get_end_char(open_file_table.file[fileID].entry.i_node_number);

    int buf_pos = 0;
    int cb = open_file_table.file[fileID].read_pointer.block;
    int cc = open_file_table.file[fileID].read_pointer.c_ptr;
    int tb = 0;
    //printf("THERE 1, cb: %d cc: %d; lb: %d, lc: %d\n", cb, cc, lb, lc);
    FILL_BLOCK:
    //End of block?
    if(cc >= NUMBER_OF_BYTES_BLOCK) {
        tb = get_next_file_block(open_file_table.file[fileID].entry.i_node_number, cb);
        if(tb < 0) goto EXIT;
        cc = 0;
        cb = tb;
    }

    read_blocks(cb, 1, &data_block);

    // Copy data block to buf
    while(cc < NUMBER_OF_BYTES_BLOCK && buf_pos < length && !(cb == lb && cc >= lc)) {
        buf[buf_pos++] = data_block.c[cc++];
    }

    if(buf_pos < length && !(cb == lb && cc >= lc)) goto FILL_BLOCK;

    EXIT:
    open_file_table.file[fileID].read_pointer.block = cb;
    open_file_table.file[fileID].read_pointer.c_ptr = cc;
    return buf_pos;
}

int ssfs_remove(char* file) {
    rm_fd(file);
    return rm_file_from_dir(file, &file_system);
}

int ssfs_commit() {
    free_shadow_directory(MAX_DIRS_INCL_SHAD-1);

    for(int i = MAX_DIRS_INCL_SHAD-1; i > 0; i--) {
        file_system.directory[i] = file_system.directory[i-1];
    }

    init_dir(&file_system.directory[0]);
    restore_shadow_directory(1);
    dump_file_system_to_disk();

    return 0;
}

int ssfs_restore(int cnum) {
    if(cnum == 0) return 0;
    if(cnum < 0 || cnum >= MAX_DIRS_INCL_SHAD) {
        printf("Error, please select cnum 1 through %d", MAX_DIRS_INCL_SHAD-1);
        return -1;
    }
    free_shadow_directory(0);
    init_dir(&file_system.directory[0]);
    restore_shadow_directory(cnum);
    dump_file_system_to_disk();
}

int gnfni = 0; // ssfs_get_next_file_name index

int ssfs_get_next_file_name(char *fname) {
    int shadow = 0;
    if ( gnfni == MAX_FILES ) { gnfni = 0; return 0; }
    while(1) {
        if(file_system.directory[shadow].entry[gnfni].name[0] != '\0') {
            strcpy(fname,file_system.directory[shadow].entry[gnfni].name);
            gnfni++;
            return 0;
        }
        if ( ++gnfni == MAX_FILES ) { gnfni = 0; return 0; }
    }
    return -1;
}

int ssfs_get_file_size(char* path) {
    for(int i = 0; i < MAX_FILES; i++) {
        if(!strncmp(file_system.directory[0].entry[i].name, path, MAX_NAME_LENGTH)){
            int i_node_number = file_system.directory[0].entry[i].i_node_number;
            int i_block       = node_number_to_block(i_node_number);
            int i_node        = node_number_to_node_in_block(i_node_number);

            return file_system.i_node_file.block[i_block].i_node[i_node].size;
        }
    }
    return -1;
}

#if 0
int main() {
    mkssfs(1);
    printf("Pre-commit\n");
    ssfs_fopen("eh1");
    print_directory(0);
    print_directory(1);
    ssfs_commit();
    printf("Post-commit\n");
    print_directory(0);
    print_directory(1);
    ssfs_restore(1);
    printf("Post-restore\n");
    print_directory(0);
    print_directory(1);
    close_disk();
    return 0;
}
#endif
