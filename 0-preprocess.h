#ifndef PREPROCESS_H
#define PREPROCESS_H

extern "C" {
    void preprocess(
	        struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        unsigned *input_all_peds, // Read-Only Pedestals
            int * bounds, // Read-Only Integral Bounds
            int *zero_thresholds, // Read-Only Thresholds for zero-suppression
	        int *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	);
}

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N
#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)
#define NUM_INTEGRALS 4
#define NUM_ALPHAS 5

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

struct Centroid {
    uint16_t position;
    uint16_t signal;
    int16_t count;
};


#endif