#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"

int16_t ped_sub_results[NUM_SAMPLES][NUM_CHANNELS]; // Really 13 bits


int ped_subtract(SW_Data_Packet * pkt, uint16_t *peds) {
    // calculate base address for integral
    for (unsigned s = 0; s < NUM_SAMPLES; ++s) {
        unsigned idx = (pkt->starting_sample_number + s) % NUM_SAMPLES;
        for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            unsigned ped_idx = pkt->bank*NUM_SAMPLES*NUM_CHANNELS + idx*NUM_CHANNELS + c;
            ped_sub_results[s][c] = pkt->samples[s][c] - peds[ped_idx];
        }
    }
    return 0;
}

int integrate(SW_Data_Packet * pkt, int *bounds, int32_t *integrals) {

    //Assume fine_time > starting_sample_number, so base_addr is positive
    int base_addr = pkt->fine_time - pkt->starting_sample_number;

    for (int s = 0; s < NUM_SAMPLES; ++s) {
        int x = s - base_addr;
        for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            for (unsigned i = 0; i < NUM_INTEGRALS; ++i) {
                int start = bounds[2*i];
                int end = bounds[2*i+1];
                if((x >= start && x <= end) || (x - NUM_SAMPLES) >= start) {
                    // printf("sample %d, base_addr %d, offset %d inside bounds [%d,%d] for integral %d\n", s, base_addr, x,
                    // bounds[2*i], bounds[2*i+1], i);
                    integrals[i*NUM_CHANNELS+c] += ped_sub_results[s][c];
                }
            }
        }
    }

    return 0;
}


int integrate_bad(int8_t* base_addr, int rel_start, int rel_end, int integral_num, int32_t * integrals) {
    // int start = data_packet->fine_time + rel_start - data_packet->starting_sample_number;
    int start = *base_addr + rel_start;
    if (start < 0) {
        start = start + NUM_SAMPLES - 1;
    }
    // int end = data_packet->fine_time + rel_end - data_packet->starting_sample_number;
    int end = *base_addr + rel_end;
    if (end >= NUM_SAMPLES - 1) {
        end = end - (NUM_SAMPLES - 1);
    }
    int linear = 0;
    if (end >= start) {
        linear = 1;
    }
    int32_t temp_integrals[4][NUM_CHANNELS];
    int32_t current_integral;
    int i_gte_start = 0;
    int i_lte_end = 0;
    
    integral_l0: for (int i = 0; i < NUM_SAMPLES; i++) {
        integral_l1: for (int j = 0; j < NUM_CHANNELS; j++) {
            #pragma HLS PIPELINE II=1

            current_integral = (i>0) ? temp_integrals[integral_num][j] : 0;
            i_gte_start = (i >= start);
            i_lte_end = (i <= end);
            if ((i_gte_start && i_lte_end) || (!linear && (i_gte_start || i_lte_end)) ) {
        if(j == 0)
        printf("sample %d, base_addr %d, inside bounds [%d,%d] for integral %d\n", i, *base_addr, 
        start, end, integral_num);
                temp_integrals[integral_num][j] = current_integral + ped_sub_results[i][j];
            }
            else {
                 temp_integrals[integral_num][j] = current_integral;
            }
            
            // #pragma HLS DEPENDENCE variable=temp_integrals false
            // I think this should be fine, since it's what they do in the example and it's a WAR dependency...
        }
    }
    // Need to transfer from the temporary buffer to the output
    integral_out_l0: for (int i = 0; i < NUM_CHANNELS; i++) {
        integrals[integral_num*NUM_CHANNELS+i] = temp_integrals[integral_num][i];
    }
    return 0;
}


extern "C" {
    void preprocess(
	        struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        uint16_t *input_all_peds, // Read-Only Pedestals
            int * bounds, // Read-Only Integral Bounds
	        int32_t *output_integrals       // Output Result (Integrals)
	        )
    {
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE m_axi depth=8192 port=input_all_peds bundle=aximm2
#pragma HLS INTERFACE m_axi depth=8 port=bounds bundle=aximm3
#pragma HLS INTERFACE m_axi depth=64 port=output_integrals bundle=aximm4


	    ped_subtract(input_data_packet, input_all_peds);

        integrate(input_data_packet, bounds, output_integrals);

        
        // int8_t base_addr = input_data_packet->fine_time - input_data_packet->starting_sample_number;
        // integrate_bad(&base_addr, bounds[0], bounds[1], 0, output_integrals);
        // integrate_bad(&base_addr, bounds[2], bounds[3], 1, output_integrals);
        // integrate_bad(&base_addr, bounds[4], bounds[5], 2, output_integrals);
        // integrate_bad(&base_addr, bounds[6], bounds[7], 3, output_integrals);
        

    }
}
