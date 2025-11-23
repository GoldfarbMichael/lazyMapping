#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <mastik/l3.h>

#define MAX_NUM_GROUPS 64
#define LINE_SIZE 64
#define CLCOCK_SPEED 3.1e9 // 3.1 GHz

// Linked list node for addresses
typedef struct addr_node {
    uint8_t *addr;
    struct addr_node *next;
} addr_node_t;

// Group structure with linked list
typedef struct {
    addr_node_t *head; 
    addr_node_t *tail;     
    size_t count;          // Number of nodes
} group_t;

// Experiment configuration
typedef struct {
    const char *name;
    int num_groups;
    int prime_enabled;
} experiment_config_t;

static inline void maccessMy(void *p) {
    __asm__ volatile("movb (%0), %%al" : : "r"(p) : "eax", "memory");
}

// Function declarations
void prepareL3(l3pp_t *l3);
group_t* initialize_groups(size_t arena_mb, void **arena_ptr, size_t *num_pages_ptr);
group_t* merge_groups_create_new(group_t *orig, int num_groups);
void cleanup_groups(group_t *groups, void *arena);
void cleanup_merged_groups(group_t *groups, int num_groups);
void randomize_group_list(group_t *group);
void shuffle_array(uint8_t **array, size_t n);

#endif