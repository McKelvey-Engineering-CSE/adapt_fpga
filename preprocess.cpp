#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"

int16_t ped_sub_results[NUM_ALPHAS][NUM_SAMPLES][NUM_CHANNELS]; // Really 13 bits


void ped_subtract(const SW_Data_Packet * pkt, const uint16_t *peds, const uint8_t a) {
    ped_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
        // calculate base address for integral
        const uint16_t idx = (pkt->starting_sample_number + s) % NUM_SAMPLES;
        ped_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            const uint16_t ped_idx = pkt->bank*NUM_SAMPLES*NUM_CHANNELS + idx*NUM_CHANNELS + c;
            ped_sub_results[a][s][c] = pkt->samples[s][c] - peds[ped_idx];
        }
    }
}

void integrate(const SW_Data_Packet * pkt, const int16_t *bounds, int32_t *integrals, const uint8_t a) {

    //Assume fine_time > starting_sample_number, so base_addr is positive
    int16_t base_addr = pkt->fine_time - pkt->starting_sample_number;
    if(base_addr < 0) base_addr += NUM_SAMPLES;

    int_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
        const int16_t x = s - base_addr;
        int_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            int_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
                const int16_t start = bounds[2*i];
                const int16_t end = bounds[2*i+1];
                if((x >= start && x <= end) || (x - NUM_SAMPLES) >= start) {
                    // printf("sample %d, base_addr %d, offset %d inside bounds [%d,%d] for integral %d\n", s, base_addr, x,
                    // bounds[2*i], bounds[2*i+1], i);
                    integrals[i*NUM_CHANNELS+c] += ped_sub_results[a][s][c];
                }
            }
        }
    }
}

void zero_suppress(int32_t * integrals, const int32_t * thresholds) {
    zero_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            if(integrals[i*NUM_CHANNELS+c] < thresholds[i]) {
                integrals[i*NUM_CHANNELS+c] = 0;
            }
        }
    }
}

int16_t island_detection(int32_t * integrals, const uint8_t integral_num) {
    bool in_island = 0;
    int16_t num_islands = 0;
    island_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
        island_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            const uint16_t idx = a * NUM_INTEGRALS * NUM_CHANNELS + 
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

int16_t centroiding(Centroid * centroid, int32_t *integrals, const uint8_t integral_num) {
    int16_t count = island_detection(integrals, integral_num);
    if (count > 0) {        
        centroiding_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            centroiding_channels: for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
                const uint16_t pos = a * NUM_CHANNELS + c;
                const uint16_t idx = a * NUM_INTEGRALS * NUM_CHANNELS + 
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
	        const struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        const uint16_t *input_all_peds, // Read-Only Pedestals
            const int16_t * bounds, // Read-Only Integral Bounds
            const int32_t *zero_thresholds, // Read-Only Thresholds for zero-suppression
	        int32_t *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE m_axi depth=5 port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE mode=bram depth=40960 port=input_all_peds
#pragma HLS INTERFACE mode=bram depth=8 port=bounds
#pragma HLS INTERFACE mode=bram depth=4 port=zero_thresholds
#pragma HLS INTERFACE m_axi depth=320 port=output_integrals bundle=aximm5
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm6

        loop_alphas: for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
            const uint16_t ped_offset = alpha * 2 * NUM_SAMPLES * NUM_CHANNELS;
            const uint16_t integral_offset = alpha * NUM_INTEGRALS * NUM_CHANNELS;

            ped_subtract(&input_data_packet[alpha],
                         input_all_peds + ped_offset,
                         alpha);

            integrate(&input_data_packet[alpha],
                      bounds,
                      output_integrals + integral_offset,
                      alpha);
            
            zero_suppress(output_integrals + integral_offset, zero_thresholds);

        }

        centroid->count = centroiding(centroid, output_integrals, 3);

    }
}
