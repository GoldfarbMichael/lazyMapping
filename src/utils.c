#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <mastik/impl.h>



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

