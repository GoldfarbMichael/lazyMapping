#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <mastik/impl.h>


// Fisher-Yates shuffle algorithm
void shuffle_array(uint8_t **array, size_t n) {
    if (n > 1) {
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rand() % (i + 1);
            uint8_t *temp = array[i];
            array[i] = array[j];
            array[j] = temp;
        }
    }
}


void prepareL3(l3pp_t *l3) {
    l3info_t l3i = (l3info_t)malloc(sizeof(struct l3info));
    if (!l3i) {
        fprintf(stderr, "Failed to allocate l3info\n");
        return;
    }
    
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;

    // Free any existing L3 instance before the loop
    if (*l3) {
        l3_release(*l3);
        *l3 = NULL;
    }

    start_cycles = rdtscp64();
    
    while (!(*l3) || l3_getSets(*l3) != EXPECTED_NUM_SETS) {
        printf("Preparing L3...\n");
        
        // Release previous attempt if it exists
        if (*l3) {
            l3_release(*l3);
            *l3 = NULL;
        }
        
        *l3 = l3_prepare(l3i, NULL);
        
        if (!(*l3)) {
            fprintf(stderr, "l3_prepare failed\n");
            break;
        }
    }
    
    end_cycles = rdtscp64();
    
    double time_cycles = (double)((end_cycles - start_cycles) / CLCOCK_SPEED) * 1e3; // in ms
    printf("L3 preparation took VIA CYCLES: %.3f ms\n", time_cycles);

    if (*l3) {
        printf("L3 Cache Sets: %d\n", l3_getSets(*l3));
        printf("L3 Cache Slices: %d\n", l3_getSlices(*l3));
        printf("L3 Cache num of lines: %d\n", l3_getAssociativity(*l3));
    }

    free(l3i);
}

group_t* initialize_groups(size_t arena_mb, void **arena_ptr, size_t *num_pages_ptr) {
    const size_t MB = 1024 * 1024;
    size_t arena_size = arena_mb * MB;

    // Seed random number generator
    srand(42);

    void *arena = NULL;
    if (posix_memalign(&arena, PAGE_SIZE, arena_size) != 0) {
        perror("posix_memalign");
        return NULL;
    }
    memset(arena, 0, arena_size);

    size_t num_pages = arena_size / PAGE_SIZE;
    printf("Arena: %zu MiB, pages: %zu, groups: %d\n",
           arena_mb, num_pages, MAX_NUM_GROUPS);

    group_t *groups = malloc(MAX_NUM_GROUPS * sizeof(group_t));
    if (!groups) {
        fprintf(stderr, "malloc failed for groups array\n");
        free(arena);
        return NULL;
    }

    // Initialize each group's linked list
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        groups[g].head = NULL;
        groups[g].tail = NULL;
        groups[g].count = 0;
    }

    // Build groups with linked lists
    for (size_t p = 0; p < num_pages; p++) {
        uint8_t *page_base = (uint8_t *)arena + p * PAGE_SIZE;
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            size_t off = (size_t)g * LINE_SIZE;
            uint8_t *addr = page_base + off;
            
            // Create new node
            addr_node_t *new_node = malloc(sizeof(addr_node_t));
            if (!new_node) {
                fprintf(stderr, "malloc failed for node in group %d\n", g);
                cleanup_groups(groups, arena);
                return NULL;
            }
            
            new_node->addr = addr;
            new_node->next = NULL;
            
            // Add to linked list (append to tail)
            if (groups[g].head == NULL) {
                groups[g].head = new_node;
                groups[g].tail = new_node;
            } else {
                groups[g].tail->next = new_node;
                groups[g].tail = new_node;
            }
            groups[g].count++;
        }
    }

    // Randomize each group's linked list
    printf("Randomizing group lists...\n");
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        randomize_group_list(&groups[g]);
    }

    *arena_ptr = arena;
    *num_pages_ptr = num_pages;
    return groups;
}


group_t* merge_groups_create_new(group_t *orig, int num_groups) {
    if (num_groups <= 0 || num_groups > 64 || (64 % num_groups) != 0) {
        fprintf(stderr, "num_groups must divide 64\n");
        return NULL;
    }

    int block = 64 / num_groups;
    group_t *merged = calloc(num_groups, sizeof(group_t));
    if (!merged) {
        perror("calloc merged groups");
        return NULL;
    }

    // Initialize merged groups
    for (int ng = 0; ng < num_groups; ng++) {
        merged[ng].head = NULL;
        merged[ng].tail = NULL;
        merged[ng].count = 0;
    }

    // Build merged groups
    for (int ng = 0; ng < num_groups; ng++) {
        int start = ng * block;
        int end = (ng + 1) * block;

        // Copy all nodes from original groups to merged group
        for (int og = start; og < end; og++) {
            addr_node_t *current = orig[og].head;
            while (current != NULL) {
                // Create new node
                addr_node_t *new_node = malloc(sizeof(addr_node_t));
                if (!new_node) {
                    perror("malloc merged node");
                    cleanup_merged_groups(merged, num_groups);
                    return NULL;
                }
                
                new_node->addr = current->addr;
                new_node->next = NULL;
                
                // Add to merged group
                if (merged[ng].head == NULL) {
                    merged[ng].head = new_node;
                    merged[ng].tail = new_node;
                } else {
                    merged[ng].tail->next = new_node;
                    merged[ng].tail = new_node;
                }
                merged[ng].count++;
                
                current = current->next;
            }
        }

        // Randomize the merged group
        randomize_group_list(&merged[ng]);
    }

    return merged;
}



void cleanup_groups(group_t *groups, void *arena) {
    if (groups) {
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            addr_node_t *current = groups[g].head;
            while (current != NULL) {
                addr_node_t *next = current->next;
                free(current);
                current = next;
            }
        }
        free(groups);
    }
    if (arena) {
        free(arena);
    }
}


void cleanup_merged_groups(group_t *groups, int num_groups) {
    if (groups) {
        for (int g = 0; g < num_groups; g++) {
            addr_node_t *current = groups[g].head;
            while (current != NULL) {
                addr_node_t *next = current->next;
                free(current);
                current = next;
            }
        }
        free(groups);
    }
}

// Convert linked list to randomized order
void randomize_group_list(group_t *group) {
    if (group->count == 0) return;

    // Create temporary array with all addresses
    uint8_t **temp_array = malloc(group->count * sizeof(uint8_t *));
    if (!temp_array) {
        fprintf(stderr, "Failed to allocate temporary array for randomization\n");
        return;
    }

    // Extract addresses from linked list
    addr_node_t *current = group->head;
    size_t index = 0;
    while (current != NULL) {
        temp_array[index++] = current->addr;
        current = current->next;
    }

    // Shuffle the array
    shuffle_array(temp_array, group->count);

    // Rebuild the linked list with shuffled order
    current = group->head;
    index = 0;
    while (current != NULL) {
        current->addr = temp_array[index++];
        current = current->next;
    }

    free(temp_array);
}





/**
 * Gets minimum values for each set across all iterations
 * Returns array of [set_index, min_value] pairs for non-zero minimums
 * 
 * @param res_mat: Matrix of [NUM_ITERATIONS][num_sets]
 * @param num_sets: Number of sets (columns in matrix)
 * @param out_count: Output parameter - number of non-zero minimums found
 * @return: Array of set_min_pair_t containing only non-zero minimums
 */
set_min_pair_t* get_min_values(uint16_t** res_mat, int num_sets, int* out_count) {
    int x = 0; //x = num of nonzero values in tempArr
    // 1) init tempArr[numOfSets]
    uint16_t* tempArr = (uint16_t*) calloc(num_sets, sizeof(uint16_t));
    if (!tempArr) {
        fprintf(stderr, "Failed to allocate tempArr\n");
        *out_count = 0;
        return NULL;
    }
    
    // Initialize with maximum values
    for (int s = 0; s < num_sets; s++) {
        tempArr[s] = UINT16_MAX;
    }
    
    // 2) for each set s in res_mat
    for (int s = 0; s < num_sets; s++) {
        // 2.1) find min value across all iterations
        uint16_t min_val = UINT16_MAX;
        for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
            if (res_mat[iter][s] < min_val) {
                min_val = res_mat[iter][s];
            }
            res_mat[iter][s] = 0; // reset to 0 for the next group
        }
        // 2.2) insert to tempArr to corresponding index
        tempArr[s] = min_val;
        if (min_val > 0 && min_val != UINT16_MAX) {
            x++;
        }
    }
    

    // 4) init retarray[x]
    set_min_pair_t* retArr = (set_min_pair_t*) calloc(x, sizeof(set_min_pair_t));
    if (!retArr) {
        fprintf(stderr, "Failed to allocate retArr\n");
        free(tempArr);
        *out_count = 0;
        return NULL;
    }
    
    // 5) for i in range 0...tempArr(len)
    int ret_index = 0;
    for (int i = 0; i < num_sets; i++) {
        // 5.1) if (tempArr[i] > 0)
        if (tempArr[i] > 0 && tempArr[i] != UINT16_MAX) {
            // 5.1.2) retArr[i] = [i, tempArr[i]]
            retArr[ret_index].set_index = i;
            retArr[ret_index].min_value = tempArr[i];
            ret_index++;
        }
    }
    
    free(tempArr);
    *out_count = x;
    
    // 6) return retArr
    return retArr;
}