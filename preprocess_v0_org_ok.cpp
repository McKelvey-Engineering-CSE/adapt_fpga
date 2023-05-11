#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// char: 8 bit, short: 16 bit, long: 32 bit

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N
#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)

/*
 Mimics incoming data packet in C types.
*/
struct SW_Data_Packet {
    uint16_t alpha; // Start Constant 0xA1FA
    uint8_t i2c_address; // 3 bits
    uint8_t conf_address; // 4 bits
    uint8_t bank; // 1 bit, A or B
    uint8_t fine_time; // 8 bits, sample number when trigger arrives
    uint32_t coarse_time; // 32 bits
    uint16_t trigger_number; // 16 bits
    uint8_t samples_after_trigger; // 8 bits
    uint8_t look_back_samples; // 8 bits
    uint8_t samples_to_be_read; // 8 bits
    uint8_t starting_sample_number; // 8 bits
    uint8_t number_of_missed_triggers; // 8 bits
    uint8_t state_machine_status; // 8 bits
    uint16_t samples[NUM_SAMPLES][NUM_CHANNELS]; // Variable size?
    // Looks like N samples for 16 channels (so 1 ASIC)
    uint16_t omega; // End Constant 0x0E6A
};

int16_t ped_sub_results[NUM_SAMPLES][NUM_CHANNELS] = {0}; // Really 13 bits


int ped_subtract(struct SW_Data_Packet * data_packet, uint16_t *all_peds) {
    int ped_sample_idx = data_packet->starting_sample_number;
    uint8_t bank = data_packet->bank;
    ped_subtract_l0: for (int i = 0; i < NUM_SAMPLES; i++) {
        ped_subtract_l1: for (int j = 0; j < NUM_CHANNELS; j++) {
            #pragma HLS PIPELINE II=1
            ped_sub_results[i][j] = data_packet->samples[i][j] - all_peds[bank*NUM_SAMPLES*NUM_CHANNELS + ped_sample_idx*NUM_CHANNELS + j];
            if (j==NUM_CHANNELS-1) {
                ped_sample_idx += 1;
                if (ped_sample_idx == NUM_SAMPLES) {
                    ped_sample_idx = 0;
                }
            }
        }
    }
    return 0;
}

int integral(struct SW_Data_Packet * data_packet, int rel_start, int rel_end, int integral_num, int32_t * integrals) {
    int start = data_packet->fine_time + rel_start - data_packet->starting_sample_number;
    if (start < 0) {
        start = start + NUM_SAMPLES - 1;
    }
    int end = data_packet->fine_time + rel_end - data_packet->starting_sample_number;
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
            temp_integrals[integral_num][j] = (i_gte_start && i_lte_end) || (!linear && (i_gte_start || i_lte_end)) ? current_integral + ped_sub_results[i][j] : current_integral;
            #pragma HLS DEPENDENCE variable=temp_integrals false
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
#pragma HLS INTERFACE m_axi port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE m_axi port=input_all_peds bundle=aximm2
#pragma HLS INTERFACE m_axi port=bounds bundle=aximm3
#pragma HLS INTERFACE m_axi port=output_integrals bundle=aximm1

	    ped_subtract(input_data_packet, input_all_peds);

        integral(input_data_packet, bounds[0], bounds[1], 0, output_integrals);
        integral(input_data_packet, bounds[2], bounds[3], 1, output_integrals);
        integral(input_data_packet, bounds[4], bounds[5], 2, output_integrals);
        integral(input_data_packet, bounds[6], bounds[7], 3, output_integrals);
    }
}