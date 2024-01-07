#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"

unsigned ped_sub_results[NUM_ALPHAS][NUM_SAMPLES][NUM_CHANNELS]; // Really 13 bits


void ped_subtract(SW_Data_Packet * pkt, unsigned *peds, unsigned a) {
    // calculate base address for integral
    for (unsigned s = 0; s < NUM_SAMPLES; ++s) {
        unsigned idx = (pkt->starting_sample_number + s) % NUM_SAMPLES;
        for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            unsigned ped_idx = pkt->bank*NUM_SAMPLES*NUM_CHANNELS + idx*NUM_CHANNELS + c;
            ped_sub_results[a][s][c] = pkt->samples[s][c] - peds[ped_idx];
        }
    }
}

void integrate(SW_Data_Packet * pkt, int *bounds, int *integrals, unsigned a) {

    //Assume fine_time > starting_sample_number, so base_addr is positive
    int base_addr = pkt->fine_time - pkt->starting_sample_number;
    if(base_addr < 0) base_addr += NUM_SAMPLES;

    for (int s = 0; s < NUM_SAMPLES; ++s) {
        int x = s - base_addr;
        for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            for (unsigned i = 0; i < NUM_INTEGRALS; ++i) {
                int start = bounds[2*i];
                int end = bounds[2*i+1];
                if((x >= start && x <= end) || (x - NUM_SAMPLES) >= start) {
                    // printf("sample %d, base_addr %d, offset %d inside bounds [%d,%d] for integral %d\n", s, base_addr, x,
                    // bounds[2*i], bounds[2*i+1], i);
                    integrals[i*NUM_CHANNELS+c] += ped_sub_results[a][s][c];
                }
            }
        }
    }
}


void zero_suppress(int * integrals, int * thresholds) {
    for(unsigned i = 0; i < NUM_INTEGRALS; ++i) {
        for(unsigned c = 0; c < NUM_CHANNELS; ++c) {
            if(integrals[i*NUM_CHANNELS+c] < thresholds[i]) {
                integrals[i*NUM_CHANNELS+c] = 0;
            }
        }
    }
}

int island_detection(int * integrals, unsigned integral_num) {
    bool in_island = 0;
    int num_islands = 0;
    for (unsigned a = 0; a < NUM_ALPHAS; ++a) {
        for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            unsigned idx = a * NUM_INTEGRALS * NUM_CHANNELS + 
                           integral_num * NUM_CHANNELS + c;
            if(integrals[idx] && !in_island) {
                in_island = true;
                ++num_islands;
            }
            else if (!integrals[idx] && in_island) {
                in_island = false;
            }
        }
    }

    //printf("Number of islands for integral %u: %d\n", integral_num, num_islands);
    return num_islands;

}

int centroiding(Centroid * centroid, int *integrals, unsigned integral_num) {
    int count = island_detection(integrals, integral_num);
    if (count > 0) {        
        for (unsigned a = 0; a < NUM_ALPHAS; ++a) {
            for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
                unsigned pos = a * NUM_CHANNELS + c;
                unsigned idx = a * NUM_INTEGRALS * NUM_CHANNELS + 
                            integral_num * NUM_CHANNELS + c;
                centroid->position += pos * integrals[idx];
                centroid->signal += integrals[idx];
            }
        }

        // printf("Centroid signal %u, mult-accum %u, ", centroid->signal, centroid->position);
        centroid->position /= centroid->signal;
        // printf("position %u\n", centroid->position);
    }
    return count;
}

extern "C" {
    void preprocess(
	        struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        unsigned *input_all_peds, // Read-Only Pedestals
            int * bounds, // Read-Only Integral Bounds
            int *zero_thresholds, // Read-Only Thresholds for zero-suppression
	        int *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE m_axi depth=5 port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE m_axi depth=40960 port=input_all_peds bundle=aximm2
#pragma HLS INTERFACE m_axi depth=8 port=bounds bundle=aximm3
#pragma HLS INTERFACE m_axi depth=4 port=zero_thresholds bundle=aximm4
#pragma HLS INTERFACE m_axi depth=320 port=output_integrals bundle=aximm5
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm6

        for (unsigned alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
            unsigned ped_offset = alpha * 2 * NUM_SAMPLES * NUM_CHANNELS;
            unsigned integral_offset = alpha * NUM_INTEGRALS * NUM_CHANNELS;

            ped_subtract(&input_data_packet[alpha],
                         input_all_peds + ped_offset,
                         alpha);

            integrate(&input_data_packet[alpha],
                      bounds,
                      output_integrals + integral_offset,
                      alpha);
            
            zero_suppress(output_integrals + integral_offset, zero_thresholds);

        }

        centroiding(centroid, output_integrals, 3);

    }
}
