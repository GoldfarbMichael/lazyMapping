#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>      // for pipe, fork, close, read, write
#include <sys/wait.h>    // for wait
#include <sys/types.h>   // for pid_t
#include <sys/mman.h>     // for shared memory
#include <semaphore.h>    // for semaphores
#include <fcntl.h>        // for O_* constants
#include <mastik/l3.h>
#include <mastik/impl.h>
#include "utils.h"


// Experiment modes: 1=NEW, 2=PRIME_BY_GROUP_LINE, 3=OLD, 4=MISSES, 0=function testing
#define EXPERIMENT_MODE 2
#define TESTING_MODE 1
// #define PRIME_BY_GROUP_LINE 0
// #define OLD_EXPERIMENT 0
// #define MISSES_EXPERIMENT 0
#define DEFAULT_ARENA_MB 24
#define OUTPUT_BASE_DIR "data/lfence"


void old_experiment(l3pp_t l3, group_t *groups, experiment_config_t *experiments, int num_experiments, const char *output_dir) {
    uint16_t* res = (uint16_t*) calloc(l3_getSets(l3), sizeof(uint16_t));
    
    // monitor all sets 
    for(int i = 0; i < l3_getSets(l3); i++){
        l3_monitor(l3, i);
        if(i % 1024 == 0){
            printf("Monitoring set %d/%d\n", i, l3_getSets(l3));
        }
    }

    // Run each experiment
    for (int exp = 0; exp < num_experiments; exp++) {
        experiment_config_t *config = &experiments[exp];
        
        printf("Running experiment: %s (groups: %d, prime: %s)\n", 
               config->name, config->num_groups, 
               config->prime_enabled ? "yes" : "no");

        // Create merged groups for this experiment
        group_t *exp_groups = merge_groups_create_new(groups, config->num_groups);
        if (!exp_groups) {
            printf("merge failed for %s\n", config->name);
            continue;
        }

        // Create log file
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s.jsonl", output_dir, config->name);
        FILE *log = fopen(filename, "w");
        
        if (!log) {
            fprintf(stderr, "Failed to open log file %s\n", filename);
            cleanup_merged_groups(exp_groups, config->num_groups);
            continue;
        }

        // Run the experiment
        for(int g = 0; g < config->num_groups; g++){
            for(int iter = 0; iter < 30; iter++){
                printf("Group %d, Iteration %d\n", g, iter);
                l3_bprobecount(l3, res);
                __asm__ volatile("lfence" ::: "memory");

                // __asm__ volatile("mfence" ::: "memory");

                // Prime only if enabled
                if (config->prime_enabled) {
                    addr_node_t *current = exp_groups[g].head;
                    while (current != NULL && current->next != NULL) {
                        maccessMy(current->next->addr);
                        maccessMy(current->addr);
                        maccessMy(current->next->addr);
                        maccessMy(current->addr);
                        current = current->next;
                    }
                }
                __asm__ volatile("lfence" ::: "memory");

                // __asm__ volatile("mfence" ::: "memory");
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
                fflush(log);
            }
        }

        fclose(log);
        cleanup_merged_groups(exp_groups, config->num_groups);
        printf("Completed experiment: %s\n", config->name);
    }
    
    free(res);
}


void only_misses_exp(l3pp_t l3, l3pp_t l3_primer, const char *output_dir) {
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    uint16_t* primer_res = (uint16_t*) calloc(l3_getSlices(l3_primer)*2, sizeof(uint16_t));
    // uint16_t* primer_res = (uint16_t*) calloc(l3_getSets(l3_primer), sizeof(uint16_t));
    
    uint16_t* finalRes = (uint16_t*) calloc(l3_getSets(l3), sizeof(uint16_t));
    
    // l3_unmonitorall(l3_primer);
    // for(int set = 0; set < l3_getSets(l3); set++){
    // l3_monitor(l3_primer, set);
    // }

    for(int set = 0; set < l3_getSets(l3); set++){
        l3_unmonitorall(l3);
        l3_unmonitorall(l3_primer);
        
        l3_monitor(l3, set); 
        for(int slice = 0; slice < l3_getSlices(l3_primer);slice++){
        l3_monitor(l3_primer, set*slice);
        }
        
        l3_bprobecount(l3, res);
        __asm__ volatile("lfence" ::: "memory");
        l3_repeatedprobecount(l3_primer, 2,primer_res, 3000);  // primer probe to evict from cache
        __asm__ volatile("lfence" ::: "memory");
        l3_probecount(l3, res);
        finalRes[set] = res[0];
    }
    // Write to JSONL log
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/misses.jsonl", output_dir);
    FILE *log = fopen(filename, "w");
    
    if (!log) {
        fprintf(stderr, "Failed to open log file %s\n", filename);
        free(finalRes);
        free(res);  
        return;
    }
    fprintf(log, "{\"group\":0,\"iter\":0,\"probe_counts\":[");
    for (int set = 0; set < l3_getSets(l3); set++) {
        fprintf(log, "%u", finalRes[set]);
        if (set < l3_getSets(l3) - 1) {
            fprintf(log, ",");
        }
    }
    fprintf(log, "]}\n");
    fclose(log);
    free(finalRes);
    free(res);
}





void new_experiment(l3pp_t l3, group_t *groups, experiment_config_t *experiments, int num_experiments, const char *output_dir) {
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    uint16_t* finalRes = (uint16_t*) calloc(l3_getSets(l3), sizeof(uint16_t));
    

    // Run each experiment
    for (int exp = 0; exp < num_experiments; exp++) {
        experiment_config_t *config = &experiments[exp];
        
        printf("Running experiment: %s (groups: %d, prime: %s)\n", 
               config->name, config->num_groups, 
               config->prime_enabled ? "yes" : "no");

        // Create merged groups for this experiment
        group_t *exp_groups = merge_groups_create_new(groups, config->num_groups);
        if (!exp_groups) {
            printf("merge failed for %s\n", config->name);
            continue;
        }

        // Create log file with directory structure
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s.jsonl", output_dir, config->name);
        FILE *log = fopen(filename, "w");
        
        if (!log) {
            fprintf(stderr, "Failed to open log file %s\n", filename);
            cleanup_merged_groups(exp_groups, config->num_groups);
            continue;
        }

        // Run the experiment
        for(int g = 0; g < config->num_groups; g++){
            for(int iter = 0; iter < 100; iter++){
                // printf("Group %d, Iteration %d\n", g, iter);
                for(int set = 0; set < l3_getSets(l3); set++){
                    l3_unmonitorall(l3);
                    l3_monitor(l3, set);
                    l3_bprobecount(l3, res);

                    __asm__ volatile("mfence" ::: "memory");

                    // Prime only if enabled
                    if (config->prime_enabled) {
                        addr_node_t *current = exp_groups[g].head;
                        while (current != NULL) {
                            maccessMy(current->addr);
                            maccessMy(current->addr);
                            maccessMy(current->addr);
                            // maccessMy(current->addr);

                            current = current->next;
                        }
                    }
                    __asm__ volatile("mfence" ::: "memory");
                    l3_probecount(l3, res);
                    finalRes[set] = res[0];
                }

                // Write to JSONL log
                fprintf(log, "{\"group\":%d,\"iter\":%d,\"probe_counts\":[", g, iter);
                for (int set = 0; set < l3_getSets(l3); set++) {
                    fprintf(log, "%u", finalRes[set]);
                    if (set < l3_getSets(l3) - 1) {
                        fprintf(log, ",");
                    }
                }
                fprintf(log, "]}\n");
                fflush(log);
            }
        }

        fclose(log);
        cleanup_merged_groups(exp_groups, config->num_groups);
        printf("Completed experiment: %s\n", config->name);
    }
    free(finalRes);
    free(res);
}


void calculate_avg_monitor_and_bprobe_time(l3pp_t l3) {
    int num_sets = l3_getSets(l3);
    uint64_t* times = (uint64_t*) calloc(num_sets, sizeof(uint64_t));
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    uint16_t* final_res = (uint16_t*) calloc(num_sets, sizeof(uint16_t));
    
    const int ITERATIONS = 100;
    double sum_avg_time_us = 0.0;
    double sum_avg_misses = 0.0;
    for(int iter = 0; iter < ITERATIONS; iter++){
        uint64_t start_cycles, end_cycles;
        // l3_unmonitorall(l3);
        // l3_monitor(l3, 0); // Dummy monitor to stabilize
        for(int i = 0; i < num_sets; i++){
            l3_unmonitorall(l3);
            start_cycles = rdtscp64();
            l3_monitor(l3, i);
            end_cycles = rdtscp64();
            l3_bprobecount(l3, res);
            times[i] = (uint64_t)(end_cycles - start_cycles);
            final_res[i] = res[0];
        }
        uint64_t total_time_cycles = 0;
        uint16_t total_probes = 0;
        for(int i = 0; i < num_sets; i++){
            total_time_cycles += times[i];
            total_probes += final_res[i];
        }
        double avg_time_cycles = (double)total_time_cycles / num_sets;
        double avg_time_us = (double)((avg_time_cycles)/CLCOCK_SPEED)*1e6; // in us
        double avg_misses = (double)total_probes / num_sets;
        sum_avg_time_us += avg_time_us;
        sum_avg_misses += avg_misses;
    }
    double avg_time_us_total = sum_avg_time_us / ITERATIONS;
    double avg_misses_total = sum_avg_misses / ITERATIONS;
    printf("Average MONITOR + BPROBE time per set: %.3f us\n", avg_time_us_total);
    printf("Average misses per set: %.3f\n", avg_misses_total);
    
}

// void prime_by_group_line(l3pp_t l3, group_t *groups, experiment_config_t *experiments, int num_experiments, const char *output_dir) {
//     uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
//     // Allocate array of pointers for NUM_ITERATIONS
//     uint16_t** finalRes = (uint16_t**) calloc(NUM_ITERATIONS, sizeof(uint16_t*));
//     if (!finalRes) {
//         fprintf(stderr, "Failed to allocate finalRes array\n");
//         free(res);
//         free(finalRes);
//         return;
//     }
    
//     // Allocate each array inside
//     for (int i = 0; i < NUM_ITERATIONS; i++) {
//         finalRes[i] = (uint16_t*) calloc(l3_getSets(l3), sizeof(uint16_t));
//         if (!finalRes[i]) {
//             fprintf(stderr, "Failed to allocate finalRes[%d]\n", i);
//             // Free previously allocated arrays
//             for (int j = 0; j < i; j++) {
//                 free(finalRes[j]);
//             }
//             free(res);
//             free(finalRes);
//             return;
//         }
//     }


    

//     // Run each experiment
//     for (int exp = 0; exp < num_experiments; exp++) {
//         experiment_config_t *config = &experiments[exp];
        
//         printf("Running experiment: %s (groups: %d, prime: %s)\n", 
//                config->name, config->num_groups, 
//                config->prime_enabled ? "yes" : "no");

//         // Create merged groups for this experiment
//         group_t *exp_groups = merge_groups_create_new(groups, config->num_groups);
//         if (!exp_groups) {
//             printf("merge failed for %s\n", config->name);
//             continue;
//         }

//         // Create log file with directory structure
//         char filename[512];
//         snprintf(filename, sizeof(filename), "%s/%s.jsonl", output_dir, config->name);
//         FILE *log = fopen(filename, "w");
        
//         if (!log) {
//             fprintf(stderr, "Failed to open log file %s\n", filename);
//             cleanup_merged_groups(exp_groups, config->num_groups);
//             continue;
//         }

//         uint64_t start_cycles, end_cycles;
//         // Run the experiment
//         for(int g = 0; g < config->num_groups; g++){

//             addr_node_t *current = exp_groups[g].head;
//             int lineCount = 0;
//             while (current != NULL)
//             { 
//                 printf("Group %d, groupLine: %d\n", g, lineCount);        
//                 start_cycles = rdtscp64();
       
//                 for(int iter = 0; iter < NUM_ITERATIONS; iter++){

//                     for(int set = 0; set < l3_getSets(l3); set++){
//                         l3_unmonitorall(l3);
//                         l3_monitor(l3, set);
//                         l3_bprobecount(l3, res);

//                         __asm__ volatile("mfence" ::: "memory"); 
                           
//                         maccessMy(current->addr);
                        
//                         __asm__ volatile("mfence" ::: "memory");
//                         l3_probecount(l3, res);
//                         finalRes[iter][set] = res[0];
//                     }
//                 }
//                 int num_nonzero;
//                 set_min_pair_t* min_values = get_min_values(finalRes, l3_getSets(l3), &num_nonzero);

//                 // Write to JSONL log
//                 fprintf(log, "{\"group\":%d,\"groupLine\":%d,\"missed_sets\":[", g, lineCount);

//                 if (min_values) {
//                     for (int i = 0; i < num_nonzero; i++) {
//                         fprintf(log, "[%d,%u]", min_values[i].set_index, min_values[i].min_value);
//                         if (i < num_nonzero - 1) {
//                             fprintf(log, ",");
//                         }
//                     }
//                     free(min_values);
//                 }

//                 fprintf(log, "]}\n");
//                 fflush(log);

//                 current = current->next;
//                 lineCount++;
//                 end_cycles = rdtscp64();
//                 double time_cycles = (double)((end_cycles - start_cycles)/CLCOCK_SPEED)*1e3; // in ms
//                 printf("group %d, groupLine %d took: %.3f ms\n", g, lineCount, time_cycles);
//             }
//         }

//         fclose(log);
//         cleanup_merged_groups(exp_groups, config->num_groups);
//         printf("Completed experiment: %s\n", config->name);
//     }
//     free(finalRes);
//     free(res);
// }

void prime_by_group_line(l3pp_t l3, group_t *groups, experiment_config_t *experiments, int num_experiments, const char *output_dir) {
    uint16_t* res = (uint16_t*) calloc(1, sizeof(uint16_t));
    
    // OPTIMIZATION: Allocate a single vector instead of a large matrix
    int num_sets = l3_getSets(l3);
    uint16_t* min_res = (uint16_t*) malloc(num_sets * sizeof(uint16_t));
    if (!min_res) {
        fprintf(stderr, "Failed to allocate min_res\n");
        free(res);
        return;
    }

    // Run each experiment
    for (int exp = 0; exp < num_experiments; exp++) {
        experiment_config_t *config = &experiments[exp];
        
        printf("Running experiment: %s (groups: %d, prime: %s)\n", 
               config->name, config->num_groups, 
               config->prime_enabled ? "yes" : "no");

        // Create merged groups for this experiment
        group_t *exp_groups = merge_groups_create_new(groups, config->num_groups);
        if (!exp_groups) {
            printf("merge failed for %s\n", config->name);
            continue;
        }

        // Create log file
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s.jsonl", output_dir, config->name);
        FILE *log = fopen(filename, "w");
        
        if (!log) {
            fprintf(stderr, "Failed to open log file %s\n", filename);
            cleanup_merged_groups(exp_groups, config->num_groups);
            continue;
        }

        // uint64_t start_cycles, end_cycles;
        for(int g = 0; g < config->num_groups; g++){
            addr_node_t *current = exp_groups[g].head;
            int lineCount = 0;
            while (current != NULL)
            { 
                printf("Group %d, groupLine: %d\n", g, lineCount);        
                
                // OPTIMIZATION: Reset min_res vector for this group line
                // We use UINT16_MAX so any real probe count will be smaller
                for (int i = 0; i < num_sets; i++) {
                    min_res[i] = UINT16_MAX;
                }
                // start_cycles = rdtscp64();
                for(int iter = 0; iter < NUM_ITERATIONS; iter++){
                    for(int set = 0; set < num_sets; set++){
                        // start_cycles = rdtscp64();

                        l3_unmonitorall(l3);
                        l3_monitor(l3, set);
                        l3_bprobecount(l3, res);


                        __asm__ volatile("mfence" ::: "memory"); 
                        // start_cycles = rdtscp64();
                           
                        maccessMy(current->addr);
                        
                        __asm__ volatile("mfence" ::: "memory");
                        l3_probecount(l3, res);

                        // end_cycles = rdtscp64();
                        // double time_cycles = (double)((end_cycles - start_cycles)/CLCOCK_SPEED)*1e6; // in us
                        // printf("group %d, groupLine %d MONITORING + BPROBE + PRIME + PROBE took: %.3f us\n", g, lineCount, time_cycles);
                        // OPTIMIZATION: Update min value on the fly
                        
                        if (res[0] < min_res[set]) {
                            min_res[set] = res[0];
                        }
                    }
                }

                // Write to JSONL log
                fprintf(log, "{\"group\":%d,\"groupLine\":%d,\"missed_sets\":[", g, lineCount);

                // Filter and Write directly from min_res
                // Condition matches previous logic: > 0 and != MAX
                int first = 1;
                for (int s = 0; s < num_sets; s++) {
                    if (min_res[s] > 0 && min_res[s] != UINT16_MAX) {
                        if (!first) {
                            fprintf(log, ",");
                        }
                        fprintf(log, "[%d,%u]", s, min_res[s]);
                        first = 0;
                    }
                }

                fprintf(log, "]}\n");
                fflush(log);

                current = current->next;
                lineCount++;
                // end_cycles = rdtscp64();
                // double time_cycles = (double)((end_cycles - start_cycles)/CLCOCK_SPEED)*1e3; // in ms
                // printf("group %d, groupLine %d took: %.3f ms\n", g, lineCount, time_cycles);
            }
        }

        fclose(log);
        cleanup_merged_groups(exp_groups, config->num_groups);
        printf("Completed experiment: %s\n", config->name);
    }
    
    free(min_res);
    free(res);
}
// Add function definition before main()
int create_output_directory(const char *path) {
    char command[1024];
    snprintf(command, sizeof(command), "mkdir -p %s", path);
    return system(command);
}

int main(int argc, char **argv) {

      // --- params ---
    size_t arena_mb = DEFAULT_ARENA_MB;       
    if (argc > 1) {
        arena_mb = strtoull(argv[1], NULL, 10);
        if (arena_mb == 0) {
            fprintf(stderr, "Invalid arena_mb\n");
            return 1;
        }
    }
    // Create output directory path
    char output_dir[256];
    snprintf(output_dir, sizeof(output_dir), "%s/%zuMB", OUTPUT_BASE_DIR, arena_mb);
    
    // Create the directory
    if (create_output_directory(output_dir) != 0) {
        fprintf(stderr, "Failed to create output directory: %s\n", output_dir);
        return 1;
    }
    
    printf("Output directory: %s\n", output_dir);

    experiment_config_t experiments[] = {
    // {"1_group_no_prime0", 1, 0},
    // {"1_group_no_prime1", 1, 0},
    {"1_group_prime", 1, 1},
    {"2_group_prime", 2, 1},
    {"4_group_prime", 4, 1},
    {"8_group_prime", 8, 1},
    {"16_group_prime", 16, 1},
    {"32_group_prime", 32, 1}
    // {"64_group_prime", 64, 1}
    };
    int num_experiments = sizeof(experiments) / sizeof(experiments[0]);

    l3pp_t l3;
    prepareL3(&l3);


    if (TESTING_MODE == 1) {
        printf("Testing functions mode...\n");

        // calculate_avg_monitor_and_bprobe_time(l3);


        // void **eviction_sets = get_eviction_sets_via_offsets(l3);
        // (void)eviction_sets; // Suppress unused variable warning


        // for (int i = 0; i < 5; i++) {
        //     // Skip empty sets
        //     if (eviction_sets[i] == NULL) continue;

        //     void *p = eviction_sets[i];
        //     // Traverse the circular linked list
        //     do {
        //         printf("%p\n", p);
        //         p = LNEXT(p); // LNEXT dereferences the pointer to find the next node
        //     } while (p != eviction_sets[i]);

        //     printf("---------\n");
        // }


        // void **sets_a = get_eviction_sets_via_offsets(l3);
        // l3_release(l3);
        // free(eviction_sets);

        sleep(3); // Pause to read output
        l3pp_t l3_b;
        prepareL3(&l3_b);
        int numOfSets = l3_getSets(l3_b);
        void **sets_b = get_eviction_sets_via_offsets(l3_b);
        // create groups from eviction sets
        group_t* groups_a = eviction_sets_to_groups(sets_b, numOfSets);
        new_experiment(l3, groups_a, experiments, num_experiments, "data/mastik_lazyGroups/24MB");

        
        
        // int intersection = check_intersection(sets_a, sets_b, numOfSets);
        // printf("Intersection result: %d\n", intersection);


    }


    
    
    if( EXPERIMENT_MODE == 4) {
        
        l3pp_t l3_primer;
        prepareL3(&l3_primer);
        only_misses_exp(l3, l3_primer, "data");
        l3_release(l3_primer);
        return 0;
    } 

    void *arena;
    size_t num_pages;
    group_t *groups = initialize_groups(arena_mb, &arena, &num_pages); 

    if (!groups) {
        return 1;  // Error already printed
    }





    //------------------ END OF INITIALIZATION ------------------//


    switch (EXPERIMENT_MODE) {
        case 1:
            new_experiment(l3, groups, experiments, num_experiments, "data/64B_stride_L2_prefetcher/24MB_3accesses");
            break;
        case 2:
            prime_by_group_line(l3, groups, experiments, num_experiments, "data/regular_pages");
            break;
        case 3:
            old_experiment(l3, groups, experiments, num_experiments, output_dir);
            break;
        case 0:
        default: {
            printf("Test mode - doing nothing\n");
            fflush(stdout);
            break;
        }
    }

    printf("After switch\n");
    fflush(stdout);

    printf("groups=%p, arena=%p, l3=%p\n", (void*)groups, arena, (void*)l3);
    fflush(stdout);

    if (groups) {
        printf("Before cleanup_groups\n");
        fflush(stdout);
        cleanup_groups(groups, NULL);
        printf("After cleanup_groups\n");
        fflush(stdout);
    }

    if (arena) {
        printf("Before free(arena)\n");
        fflush(stdout);
        free(arena);
        printf("After free(arena)\n");
        fflush(stdout);
    }

    printf("Before l3_release\n");
    fflush(stdout);
    l3_release(l3);
    printf("After l3_release\n");
    fflush(stdout);
    
    return 0;



//     cleanup_groups(groups, NULL);  // Don't double-free arena

//     if (arena) {
//         free(arena);  // Free arena separately
//     }
//     l3_release(l3);
  
    
//     return 0;
}