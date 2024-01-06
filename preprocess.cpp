#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cstring> //For memcpy

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"

int16_t ped_sub_results[NUM_ALPHAS][NUM_SAMPLES][NUM_CHANNELS]; // Really 13 bits
int32_t integrals[NUM_ALPHAS][NUM_INTEGRALS][NUM_CHANNELS];
int32_t zeroed_integrals[NUM_ALPHAS][NUM_INTEGRALS][NUM_CHANNELS];
Centroid local_centroid;


void ped_subtract(const SW_Data_Packet * pkt, const uint16_t *peds, const uint8_t a) {

    #pragma HLS ARRAY_PARTITION variable=ped_sub_results type=complete dim=3

    const uint8_t starting_sample_number = pkt->starting_sample_number;
    const uint8_t bank = pkt->bank;

    ped_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
        ped_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            #pragma HLS PIPELINE II=1
            // calculate base address for integral
            const uint16_t idx = (starting_sample_number + s) % NUM_SAMPLES;
            const uint16_t ped_idx = bank*NUM_SAMPLES*NUM_CHANNELS + idx*NUM_CHANNELS + c;
            ped_sub_results[a][s][c] = pkt->samples[s][c] - peds[ped_idx];
        }
    }
}

void integrate(const SW_Data_Packet * pkt, const int16_t *bounds, const uint8_t a) {

    #pragma HLS ARRAY_PARTITION variable=ped_sub_results type=complete dim=3

    //Assume fine_time > starting_sample_number, so base_addr is positive
    int16_t base_addr = pkt->fine_time - pkt->starting_sample_number;
    base_addr = (base_addr < 0) ? base_addr + NUM_SAMPLES : base_addr;

    int_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
        int_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            #pragma HLS PIPELINE II=1
            const int16_t start = bounds[2*i];
            const int16_t end = bounds[2*i+1];
            int32_t current_integral = 0;
            int_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
                const int16_t x = s - base_addr;
                current_integral =
                    ((x >= start && x <= end) || (x - NUM_SAMPLES) >= start) ?
                    current_integral + ped_sub_results[a][s][c] :
                    current_integral;
            }
            integrals[a][i][c] = current_integral;
        }
    }
}

void zero_suppress(const int32_t * thresholds, const uint8_t a) {
    zero_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        #pragma HLS PIPELINE II=1
        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            int32_t integral = integrals[a][i][c];
            const int32_t threshold = thresholds[i];
            integral = (integral < threshold) ? 0 : integral;
            zeroed_integrals[a][i][c] = integral;
        }
    }
}

int16_t island_detection(const uint8_t integral_num) {
    bool in_island = 0;
    int16_t num_islands = 0;
    island_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
        #pragma HLS UNROLL factor=5
        island_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            #pragma HLS UNROLL factor=16
            int32_t integral_val = zeroed_integrals[a][integral_num][c];
            if(integral_val && !in_island) {
                in_island = true;
                ++num_islands;
            }
            else if (!integral_val && in_island) {
                in_island = false;
            }
        }
    }

    //printf("Number of islands for integral %u: %d\n", integral_num, num_islands);
    return num_islands;

}

int16_t centroiding(Centroid * centroid, const uint8_t integral_num) {
    int16_t count = island_detection(integral_num);
    uint16_t position = 0;
    uint16_t signal = 0;  
    centroiding_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
        #pragma HLS UNROLL factor=5
        centroiding_channels: for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            #pragma HLS UNROLL factor=16
            const uint16_t pos = a * NUM_CHANNELS + c;
            const int32_t integral = zeroed_integrals[a][integral_num][c];
            position += pos * integral;
            signal += integral;
        }
    }
    centroid->position = (count > 0) ? position / signal : 0;
    centroid->signal = (count > 0) ? signal : 0;
    return count;
}

void write_outputs(Centroid * centroid, int32_t * output_integrals) {
    *centroid = local_centroid;
    memcpy(output_integrals, zeroed_integrals, NUM_ALPHAS*NUM_INTEGRALS*NUM_CHANNELS*sizeof(int32_t));
}

extern "C" {
    void preprocess(
	        const struct SW_Data_Packet input_data_packet[NUM_ALPHAS], // Read-Only Data Packet Struct
	        const uint16_t *input_all_peds, // Read-Only Pedestals
            const int16_t bounds[2*NUM_INTEGRALS], // Read-Only Integral Bounds
            const int32_t *zero_thresholds, // Read-Only Thresholds for zero-suppression
	        int32_t *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE mode=bram depth=40960 port=input_all_peds
#pragma HLS INTERFACE mode=bram depth=1 port=bounds
#pragma HLS array_partition variable=bounds type=block factor=4
#pragma HLS INTERFACE mode=bram depth=4 port=zero_thresholds
#pragma HLS INTERFACE m_axi depth=320 port=output_integrals bundle=aximm5
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm6

        loop_alphas: for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {

            ped_subtract(input_data_packet + alpha,
                         input_all_peds + alpha * 2 * NUM_SAMPLES * NUM_CHANNELS,
                         alpha);

            integrate(input_data_packet + alpha,
                      bounds,
                      alpha);
            
            zero_suppress(zero_thresholds, alpha);

        }

        centroiding(&local_centroid, 3);

        write_outputs(centroid, output_integrals);

    }
}
