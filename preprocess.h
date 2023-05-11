#ifndef PREPROCESS_H
#define PREPROCESS_H


#include <hls_vector.h>
typedef hls::vector<int16_t, 16> vec_int16_16;
typedef hls::vector<uint16_t, 16> vec_uint16_16;

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N


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

extern "C" {
    void preprocess(
	        struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        vec_uint16_16 *input_all_peds, // Read-Only Pedestals
            int * bounds, // Read-Only Integral Bounds
	        int32_t *output_integrals       // Output Result (Integrals)
	);
}


#endif