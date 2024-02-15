#ifndef PREPROCESS_H
#define PREPROCESS_H

#include <hls_vector.h>
typedef hls::vector<int16_t, 16> vec_int16_16;
typedef hls::vector<uint16_t, 16> vec_uint16_16;
typedef hls::vector<int32_t, 16> vec_int32_16;
typedef hls::vector<uint32_t, 16> vec_uint32_16;

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N
#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)
#define NUM_INTEGRALS 4
#define NUM_ALPHAS 5
#define INTEGRAL_NUM 3
#define PAIR_HISTORY 4

extern "C" {
    void preprocess(
	        const struct SW_Data_Packet * input_data_packet0, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet1, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet2, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet3, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet4, // Read-Only Data Packet Struct
	        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
            const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
            const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
	        vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS],       // Output Result (Integrals)
	        vec_int32_16 pair_buffer[NUM_ALPHAS][PAIR_HISTORY], // Output pair_buffers
			vec_int32_16 output_islands[NUM_ALPHAS][NUM_INTEGRALS],
			int16_t output_num_islands
	);
}

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
    vec_uint16_16 samples[NUM_SAMPLES]; // Variable size?
    // Looks like N samples for 16 channels (so 1 ASIC)
    uint16_t omega; // End Constant 0x0E6A
};

struct Centroid {
    uint16_t position;
    uint16_t signal;
    int16_t count;
};


#endif
