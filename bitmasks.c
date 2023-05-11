#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NUM_SAMPLES 256 // N

int main(int argc, char *argv[]){
    
    if (argc != 5) {
        printf("Usage: %s <start> <end> <trigger> <starting_sample_number>\n", argv[0]);
        return -1;
    }
    
    int start = atoi(argv[1]);
    int end = atoi(argv[2]);
    int trigger = atoi(argv[3]);
    int starting_sample_number = atoi(argv[4]);
    uint64_t masks[4] = {0};

    start = trigger + start - starting_sample_number;
    if (start < 0) {
        start = start + (NUM_SAMPLES - 1);
    }
    printf("START: %d\n", start);
    end = trigger + end - starting_sample_number;
    if (end >= (NUM_SAMPLES - 1)) {
        end = end - (NUM_SAMPLES - 1);
    }
    printf("END: %d\n", end);
    int idx = 0;
    int linear = 0;
    if (end >= start) {
        linear = 1;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 64; j++) {
            idx = i*64 + j;
            masks[i] = masks[i] << 1;
            if(((idx >= start) && (idx <= end)) || (!linear && ((idx >= start) || (idx <= end)))) {
                masks[i] = masks[i] | 1;
            }
        }
    }
    printf("BITMASK COMPONENTS:\n");
    printf("%lx\n", masks[0]);
    printf("%lx\n", masks[1]);
    printf("%lx\n", masks[2]);
    printf("%lx\n", masks[3]);
    return 0;
}