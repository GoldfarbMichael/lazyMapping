#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <mastik/impl.h>



// Offsets derived from Mastik struct l3pp
#define OFFSET_MONITOREDHEAD 0
#define OFFSET_NMONITORED    8
#define OFFSET_MONITOREDSET  16



void **get_eviction_sets_via_offsets(l3pp_t l3) {
    if (!l3) return NULL;
    int total_sets = l3_getSets(l3);
    // 1. Ensure all sets are being monitored
    for (int i = 0; i < total_sets; i++)
    {
        l3_monitor(l3, i);
    }
    

    // 2. Use pointer arithmetic to access the private fields
    char *base_addr = (char *)l3;
    
    // Extract: void **monitoredhead
    void ***monitoredhead_ptr = (void ***)(base_addr + OFFSET_MONITOREDHEAD);
    void **internal_heads = *monitoredhead_ptr;

    // Extract: int nmonitored
    int *nmonitored_ptr = (int *)(base_addr + OFFSET_NMONITORED);
    int active_count = *nmonitored_ptr;
    printf("Active monitored sets: %d\n", active_count);

    // Extract: int *monitoredset (The array of actual Set IDs)
    // While strict sequential monitoring makes this optional, using it makes the code crash-proof.
    int **monitoredset_ptr = (int **)(base_addr + OFFSET_MONITOREDSET);
    int *internal_set_ids = *monitoredset_ptr;

    // 3. Allocate the dense array to return
    // l3_getSets returns the total number of sets (e.g. 2048, 8192)
    void **dense_array = (void **)calloc(total_sets, sizeof(void *));
    if (!dense_array) return NULL;

    // 4. Map the internal sparse arrays to our dense array
    for (int i = 0; i < active_count; i++) {
        // Get the real Set ID (e.g., 0, 1, 2...)
        int set_idx = internal_set_ids[i];

        // if(i % 256 == 0) {
        //     printf("i IS: %d set_idx IS: %d\n", i, set_idx);
        // }
        
        // Safety check
        if (set_idx >= 0 && set_idx < total_sets) {
            dense_array[set_idx] = internal_heads[i];
        }
    }
    l3_unmonitorall(l3);
    return dense_array;
}



// Comparator for qsort
int compare_ptrs(const void *a, const void *b) {
    const void *ptr_a = *(const void **)a;
    const void *ptr_b = *(const void **)b;
    if (ptr_a < ptr_b) return -1;
    if (ptr_a > ptr_b) return 1;
    return 0;
}

// Helper: Flattens all linked lists in 'sets' into a single sorted array of pointers
// Returns the array, and sets *out_count to the number of elements.
void **collect_and_sort_addresses(void **sets, int nsets, size_t *out_count) {
    size_t capacity = 1024;
    size_t count = 0;
    void **flat_array = malloc(capacity * sizeof(void *));

    for (int i = 0; i < nsets; i++) {
        if (sets[i] == NULL) continue;

        void *curr = sets[i];
        do {
            // Resize if necessary
            if (count >= capacity) {
                capacity *= 2;
                flat_array = realloc(flat_array, capacity * sizeof(void *));
            }
            
            // Add address
            flat_array[count++] = curr;
            
            // Move to next node
            curr = LNEXT(curr);
        } while (curr != sets[i]);
    }

    // Sort the collected addresses
    qsort(flat_array, count, sizeof(void *), compare_ptrs);
    
    *out_count = count;
    return flat_array;
}

/* * Checks if there is any intersection between the addresses in the eviction sets 
 * of l3_a and l3_b.
 * Returns: 1 if intersection found, 0 otherwise.
 * Prints the first 10 intersecting addresses found.
 */
int check_intersection(void **sets_a,void **sets_b, int numOfSets) {
    if (!sets_a || !sets_b) return 0;

    int nsets_a = numOfSets;
    int nsets_b = numOfSets;


    // 2. Flatten and sort
    size_t count_a, count_b;
    void **flat_a = collect_and_sort_addresses(sets_a, nsets_a, &count_a);
    void **flat_b = collect_and_sort_addresses(sets_b, nsets_b, &count_b);

    // 3. Find intersection (linear scan of two sorted arrays)
    size_t i = 0, j = 0;
    int found_any = 0;
    int print_count = 0;

    printf("Checking intersection between %zu addresses (A) and %zu addresses (B)...\n", count_a, count_b);

    while (i < count_a && j < count_b) {
        if (flat_a[i] < flat_b[j]) {
            i++;
        } else if (flat_a[i] > flat_b[j]) {
            j++;
        } else {
            // Match found!
            found_any = 1;
            if (print_count < 10) {
                printf("Intersection found at address: %p\n", flat_a[i]);
            }
            print_count++;
            i++;
            j++;
        }
    }

    if (found_any) {
        printf("Total intersections found: %d\n", print_count);
    } else {
        printf("No intersections found.\n");
    }

    // 4. Cleanup
    free(flat_a);
    free(flat_b);


    return found_any;
}



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

// -----------128B stride version for 64 groups--------------

// group_t* initialize_groups(size_t arena_mb, void **arena_ptr, size_t *num_pages_ptr) {
//     const size_t MB = 1024 * 1024;
//     size_t arena_size = 2 * arena_mb * MB;

//     // Seed random number generator
//     srand(42);

//     void *arena = NULL;
//     if (posix_memalign(&arena, PAGE_SIZE, arena_size) != 0) {
//         perror("posix_memalign");
//         return NULL;
//     }
//     memset(arena, 0, arena_size);

//     size_t num_pages = arena_size / PAGE_SIZE;
//     // We process memory in "Double Pages" (8KB chunks).
//     // An 8KB chunk perfectly fits 64 groups * 128 bytes (Data + Padding).
//     size_t num_double_pages = num_pages / 2;

//     printf("Arena: %zu MiB (Effective), Allocated: %zu MiB\n", arena_mb, arena_size / MB);
//     printf("Total Pages: %zu, Double-Page Blocks: %zu, Groups: %d\n",
//            num_pages, num_double_pages, MAX_NUM_GROUPS);

//     group_t *groups = malloc(MAX_NUM_GROUPS * sizeof(group_t));
//     if (!groups) {
//         fprintf(stderr, "malloc failed for groups array\n");
//         free(arena);
//         return NULL;
//     }

//     // Initialize each group's linked list
//     for (int g = 0; g < MAX_NUM_GROUPS; g++) {
//         groups[g].head = NULL;
//         groups[g].tail = NULL;
//         groups[g].count = 0;
//     }

//     // Build groups iterating over 8KB chunks (Double Pages)
//     for (size_t dp = 0; dp < num_double_pages; dp++) {
//         // Calculate the base address of this 8KB chunk
//         // dp * 2 * PAGE_SIZE ensures we step 8KB forward each iteration
//         uint8_t *chunk_base = (uint8_t *)arena + (dp * 2 * PAGE_SIZE);


//         for (int g = 0; g < MAX_NUM_GROUPS; g++) {
//             // We use a 128-byte stride (2 * LINE_SIZE) instead of 64 bytes.
//             // Layout: [Group G Data (64B)] [Padding/Garbage (64B)] [Group G+1 Data (64B)] ...
//             // The hardware prefetcher will fetch the padding (L2 Adjacent Cache Line Prefetcher).
//             size_t off = (size_t)g * (LINE_SIZE * 2);
//             uint8_t *addr = chunk_base + off;
            
//             // Create new node
//             addr_node_t *new_node = malloc(sizeof(addr_node_t));
//             if (!new_node) {
//                 fprintf(stderr, "malloc failed for node in group %d\n", g);
//                 cleanup_groups(groups, arena);
//                 return NULL;
//             }
            
//             new_node->addr = addr;
//             new_node->next = NULL;
            
//             // Add to linked list (append to tail)
//             if (groups[g].head == NULL) {
//                 groups[g].head = new_node;
//                 groups[g].tail = new_node;
//             } else {
//                 groups[g].tail->next = new_node;
//                 groups[g].tail = new_node;
//             }
//             groups[g].count++;
//         }
//     }

//     // Randomize each group's linked list
//     printf("Randomizing group lists...\n");
//     for (int g = 0; g < MAX_NUM_GROUPS; g++) {
//         randomize_group_list(&groups[g]);
//     }

//     *arena_ptr = arena;
//     *num_pages_ptr = num_pages;
//     return groups;
// }



// -----------64B stride version for 32 groups--------------

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

    // Build groups with "Sector Merging" strategy
    for (size_t p = 0; p < num_pages; p++) {
        uint8_t *page_base = (uint8_t *)arena + p * PAGE_SIZE;
        
        // We have 32 Groups. We want to cover the whole 4096 bytes.
        // Each group must cover 128 bytes (a Sector).
        // Inside that sector, we grab BOTH 64-byte lines.
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            
            // Calculate the 128-byte Sector Base
            size_t sector_base = (size_t)g * 128;
            
            // --- Line 1 (The "Even" Twin) ---
            size_t off1 = sector_base; 
            uint8_t *addr1 = page_base + off1;
            
            // --- Line 2 (The "Odd" Twin) ---
            size_t off2 = sector_base + 64;
            uint8_t *addr2 = page_base + off2;
            
            // Add BOTH to the same group list
            // This ensures we cover the whole cache (Full Coverage)
            // And gives us 2x eviction depth per page.
            
            // Node 1
            addr_node_t *node1 = malloc(sizeof(addr_node_t));
            if (!node1) { 
                fprintf(stderr, "malloc failed for node1 in group %d\n", g);
                cleanup_groups(groups, arena);
                return NULL;
            }
            node1->addr = addr1;
            node1->next = NULL;
            
            if (groups[g].head == NULL) { 
                groups[g].head = node1;
                groups[g].tail = node1; 
            }
            else {
                groups[g].tail->next = node1; 
                groups[g].tail = node1; 
            }
            groups[g].count++;

            // Node 2
            addr_node_t *node2 = malloc(sizeof(addr_node_t));
            if (!node2) { 
                fprintf(stderr, "malloc failed for node2 in group %d\n", g);
                cleanup_groups(groups, arena);
                return NULL;
             }
            node2->addr = addr2;
            node2->next = NULL;
            
            groups[g].tail->next = node2;
            groups[g].tail = node2;
            groups[g].count++;
        }
    }

    // Randomize each group's linked list
    // Crucial: Randomizing mixes the "Odd" and "Even" lines together
    // so the prefetcher is stressed in a uniform way.
    printf("Randomizing group lists...\n");
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        randomize_group_list(&groups[g]);
    }

    *arena_ptr = arena;
    *num_pages_ptr = num_pages;
    return groups;
}

group_t* merge_groups_create_new(group_t *orig, int num_groups) {
    if (num_groups <= 0 || num_groups > MAX_NUM_GROUPS || (MAX_NUM_GROUPS % num_groups) != 0) {
        fprintf(stderr, "num_groups must divide MAX_NUM_GROUPS\n");
        return NULL;
    }

    int block = MAX_NUM_GROUPS / num_groups;
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



/*
 * Converts an array of Mastik eviction sets into a group_t array.
 * Groups are determined by bits 7-11 of the address (merging adjacent lines).
 * * e_sets: Array of pointers to linked lists (from get_eviction_sets_via_offsets)
 * num_sets: Size of the e_sets array (e.g., from l3_getSets)
 */
group_t* eviction_sets_to_groups(void **e_sets, int num_sets) {
    if (!e_sets) return NULL;

    // 1. Allocate groups array
    // We assume MAX_NUM_GROUPS is defined in utils.h (usually 32 or 64)
    group_t *groups = (group_t *)malloc(MAX_NUM_GROUPS * sizeof(group_t));
    if (!groups) {
        perror("malloc groups");
        return NULL;
    }

    // Initialize groups
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        groups[g].head = NULL;
        groups[g].tail = NULL;
        groups[g].count = 0;
    }

    // 2. Iterate through all eviction sets
    for (int i = 0; i < num_sets; i++) {
        if (e_sets[i] == NULL) continue;

        void *curr = e_sets[i];
        
        // Traverse the circular linked list of the eviction set
        do {
            // 3. Calculate Group ID
            // We want 32 groups max.
            // Bits 6-11 define the 64 sets in a standard 4KB page view.
            // To group adjacent lines (Bit 6=0 and Bit 6=1) together, we use Bits 7-11.
            // Shift right by 7, mask 5 bits (0x1F = 31).
            uintptr_t addr_val = (uintptr_t)curr;
            int group_idx = (addr_val >> 7) & 0x1F;

            // Safety check against MAX_NUM_GROUPS
            if (group_idx < MAX_NUM_GROUPS) {
                // 4. Create new node for the group
                addr_node_t *new_node = (addr_node_t *)malloc(sizeof(addr_node_t));
                if (new_node) {
                    new_node->addr = curr;
                    new_node->next = NULL;

                    // Append to group
                    if (groups[group_idx].head == NULL) {
                        groups[group_idx].head = new_node;
                        groups[group_idx].tail = new_node;
                    } else {
                        groups[group_idx].tail->next = new_node;
                        groups[group_idx].tail = new_node;
                    }
                    groups[group_idx].count++;
                }
            }

            curr = LNEXT(curr); // Move to next in circular list
        } while (curr != e_sets[i]);
    }

    // 5. Print final sizes and Randomize
    printf("Eviction Set Groups Created (Mapping bits 7-11):\n");
    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        // Only print/randomize if the group is relevant (indices 0-31)
        if (g < 32) {
            printf("Group %2d: %4zu lines\n", g, groups[g].count);
            
            // Randomize to avoid stride patterns during priming
            if (groups[g].count > 0) {
                randomize_group_list(&groups[g]);
            }
        }
    }

    return groups;
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