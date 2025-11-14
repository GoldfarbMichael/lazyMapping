#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <mastik/l3.h>
#include <mastik/impl.h>

#define CLCOCK_SPEED 3.1e9 // 3.1 GHz
#define MAX_NUM_GROUPS 64
#define LINE_SIZE  64
#define NUM_OF_GROUPS 2

typedef struct {
    uint8_t **addrs;   // array of addresses in this group
    size_t    count;   // how many are used
    size_t    cap;     // capacity (for safety)
} group_t;

void prepareL3(l3pp_t *l3) {
     l3info_t l3i = (l3info_t)malloc(sizeof(struct l3info));
     uint64_t start_cycles, end_cycles;

    printf("Preparing \n");
    start_cycles = rdtscp64();
    *l3 = l3_prepare(l3i, NULL);
    end_cycles = rdtscp64();

    double time_cycles = (double)((end_cycles - start_cycles)/CLCOCK_SPEED)*1e3; // in ms
    printf("L3 preparation took VIA CYCLES: %.3f ms\n", time_cycles);

    printf("L3 Cache Sets: %d\n", l3_getSets(*l3));
    printf("L3 Cache Slices: %d\n", l3_getSlices(*l3));
    printf("L3 Cache num of lines: %d\n", l3_getAssociativity(*l3));

    free(l3i);
}

group_t* initialize_groups(size_t arena_mb, void **arena_ptr, size_t *num_pages_ptr) {
    const size_t MB = 1024 * 1024;
    size_t arena_size = arena_mb * MB;

    // align arena on PAGE_SIZE
    void *arena = NULL;
    if (posix_memalign(&arena, PAGE_SIZE, arena_size) != 0) {
        perror("posix_memalign");
        return NULL;
    }
    memset(arena, 0, arena_size); // initialize to zero

    size_t num_pages = arena_size / PAGE_SIZE;
    printf("Arena: %zu MiB, pages: %zu, groups: %d\n",
           arena_mb, num_pages, MAX_NUM_GROUPS);

    // --- init groups ---
    group_t *groups = malloc(MAX_NUM_GROUPS * sizeof(group_t));
    if (!groups) {
        fprintf(stderr, "malloc failed for groups array\n");
        free(arena);
        return NULL;
    }

    for (int g = 0; g < MAX_NUM_GROUPS; g++) {
        groups[g].cap   = num_pages;   // one addr per page
        groups[g].count = 0;
        groups[g].addrs = malloc(num_pages * sizeof(uint8_t *));
        if (!groups[g].addrs) {
            fprintf(stderr, "malloc failed for group %d\n", g);
            // Cleanup previously allocated groups
            for (int cleanup = 0; cleanup < g; cleanup++) {
                free(groups[cleanup].addrs);
            }
            free(groups);
            free(arena);
            return NULL;
        }
    }

    // --- build lazy groups ---
    for (size_t p = 0; p < num_pages; p++) {
        uint8_t *page_base = (uint8_t *)arena + p * PAGE_SIZE;
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            size_t off = (size_t)g * LINE_SIZE;  // g[0..63] - jumps of 64 bytes
            uint8_t *addr = page_base + off;
            groups[g].addrs[groups[g].count++] = addr;
        }
    }

    // Return values through pointers
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

    // Allocate NEW groups struct
    group_t *merged = calloc(num_groups, sizeof(group_t));
    if (!merged) {
        perror("calloc merged groups");
        return NULL;
    }

    // Build merged groups
    for (int ng = 0; ng < num_groups; ng++) {

        // Count total number of addresses for this merged group
        size_t total = 0;
        int start = ng * block;
        int end   = (ng + 1) * block;

        for (int og = start; og < end; og++)
            total += orig[og].count;

        // Allocate new pointer array
        merged[ng].addrs = malloc(total * sizeof(uint8_t *));
        if (!merged[ng].addrs) {
            perror("malloc merged addrs");
            // Caller will free partially created merged[] using existing cleanup
            return merged;
        }

        merged[ng].cap   = total;
        merged[ng].count = 0;

        // Copy pointers from old groups into new group
        for (int og = start; og < end; og++) {
            memcpy(&merged[ng].addrs[merged[ng].count],
                   orig[og].addrs,
                   orig[og].count * sizeof(uint8_t *));

            merged[ng].count += orig[og].count;
        }
    }

    return merged;
}



void cleanup_groups(group_t *groups, void *arena) {
    if (groups) {
        for (int g = 0; g < MAX_NUM_GROUPS; g++) {
            free(groups[g].addrs);
        }
        free(groups);
    }
    if (arena) {
        free(arena);
    }
}

void cleanup_merged_groups(group_t *groups, int num_groups) {
    if (groups) {
        for (int g = 0; g < num_groups; g++) {  // Use actual count, not MAX_NUM_GROUPS
            free(groups[g].addrs);
        }
        free(groups);
    }
}


static inline void maccessMy(void *p) {
    __asm__ volatile("movb (%0), %%al" : : "r"(p) : "eax", "memory");
}

int main(int argc, char **argv) {

      // --- params ---
    size_t arena_mb = 12;         // default ~12 MiB
    if (argc > 1) {
        arena_mb = strtoull(argv[1], NULL, 10);
        if (arena_mb == 0) {
            fprintf(stderr, "Invalid arena_mb\n");
            return 1;
        }
    }

    l3pp_t l3;
    prepareL3(&l3);
    uint16_t* res = (uint16_t*) calloc(l3_getSets(l3), sizeof(uint16_t));
    void *arena;
    size_t num_pages;
    group_t *groups = initialize_groups(arena_mb, &arena, &num_pages);
    
    if (!groups) {
        return 1;  // Error already printed
    }

    FILE *log = fopen("group_traces.jsonl", "w");
    if (!log) {
        fprintf(stderr, "Failed to open log file\n");
        cleanup_groups(groups, arena);
        l3_release(l3);
        free(res);
        return 1;
    }

    // // --- sanity print ---
    // for (int g = 0; g < MAX_NUM_GROUPS; g++) {
    //     printf("Group %2d: %zu addresses\n", g, groups[g].count);
        
    //     size_t print_count = (groups[g].count < 5) ? groups[g].count : 5;
    //     for (size_t i = 0; i < print_count; i++) {
    //         printf("  [%zu]: %p\n", i, (void*)groups[g].addrs[i]);
    //     }
    //     printf("--------\n");
    // }


    //------------------ END OF INITIALIZATION ------------------//

    // monitor all sets 
    for(int i =0; i < l3_getSets(l3); i++){
        l3_monitor(l3, i);
        if(i%1024 == 0){
            printf("Monitoring set %d/%d\n", i, l3_getSets(l3));
        }
    }

    group_t *new_groups = merge_groups_create_new(groups, NUM_OF_GROUPS);  
    if (!new_groups) {
        printf("merge failed\n");
    } else {
        // Only free the old groups structure, NOT the arena
        if (groups) {
            for (int g = 0; g < MAX_NUM_GROUPS; g++) {
                free(groups[g].addrs);
            }
            free(groups);
        }
        
        groups = new_groups;
        printf("merge success\n");
    }


    for(int g = 0; g < NUM_OF_GROUPS; g++){
        for(int iter = 0; iter < 30; iter++){
            printf("Group %d, Iteration %d\n", g, iter);
            l3_bprobecount(l3, res); // reset probe counts (traverses backwards)

            // Ensure all memory accesses complete before probing
            __asm__ volatile("mfence" ::: "memory");

            // // PRIME group g
            // for (size_t i = 0; i < groups[g].count; i++) {
            //     maccessMy(groups[g].addrs[i]);
            // }

            // Ensure all memory accesses complete before probing
            __asm__ volatile("mfence" ::: "memory");

            l3_probecount(l3, res);

            // Write to JSONL log
            fprintf(log, "{\"group\":%d,\"iter\":%d,\"probe_counts\":[", g, iter);
            for (int set = 0; set < l3_getSets(l3); set++) {
                fprintf(log, "%u", res[set]);
                if (set < l3_getSets(l3) - 1) {
                    fprintf(log, ",");
                }
            }
            fprintf(log, "]}\n");
            fflush(log); // Ensure data is written immediately
            sleep(1); 
        }
    }


    // Cleanup
    cleanup_merged_groups(groups, NUM_OF_GROUPS);
if (arena) {
    free(arena);  // Free arena separately
}
    l3_release(l3);
    free(res);
    fclose(log); 
    return 0;
}