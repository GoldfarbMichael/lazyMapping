#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <mastik/l3.h>
#include "utils.h"

#define OLD_EXPERIMENT 0
#define DEFAULT_ARENA_MB 48
#define OUTPUT_BASE_DIR "data"

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

                __asm__ volatile("mfence" ::: "memory");

                // Prime only if enabled
                if (config->prime_enabled) {
                    addr_node_t *current = exp_groups[g].head;
                    while (current != NULL) {
                        maccessMy(current->addr);
                        current = current->next;
                    }
                }

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
                fflush(log);
            }
        }

        fclose(log);
        cleanup_merged_groups(exp_groups, config->num_groups);
        printf("Completed experiment: %s\n", config->name);
    }
    
    free(res);
}


/**
 *  New experiment function template
 *  here we probe one set at a time 
 *  res is size of 1 
 *  finalRes is size of l3_getSets(l3) 
 */
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
            for(int iter = 0; iter < 30; iter++){
                printf("Group %d, Iteration %d\n", g, iter);
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

    l3pp_t l3;
    prepareL3(&l3);
    void *arena;
    size_t num_pages;
    group_t *groups = initialize_groups(arena_mb, &arena, &num_pages); 
    
    if (!groups) {
        return 1;  // Error already printed
    }

    experiment_config_t experiments[] = {
    {"1_group_no_prime0", 1, 0},
    {"1_group_no_prime1", 1, 0},
    {"1_group_prime", 1, 1},
    {"2_group_prime", 2, 1},
    {"4_group_prime", 4, 1},
    {"8_group_prime", 8, 1},
    {"16_group_prime", 16, 1},
    {"32_group_prime", 32, 1},
    {"64_group_prime", 64, 1}
    };
    int num_experiments = sizeof(experiments) / sizeof(experiments[0]);



    //------------------ END OF INITIALIZATION ------------------//


    if (OLD_EXPERIMENT == 1) {
        old_experiment(l3, groups, experiments, num_experiments, output_dir);
    } else {
        // Future: new experiment code goes here
        new_experiment(l3, groups, experiments, num_experiments, output_dir);
    }


cleanup_groups(groups, NULL);  // Don't double-free arena

if (arena) {
    free(arena);  // Free arena separately
}
    l3_release(l3);
  
    
    return 0;
}