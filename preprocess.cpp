#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ap_int.h>
#include <hls_stream.h>

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"

#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)
#define SIZE 4096 // SIZE OF FIFO STREAM
#define NUM_INTEGRALS 4


static void read_params(const struct SW_Data_Packet* data_packet, uint8_t &bank,
		uint8_t &start_sample_number, int8_t &base_addr) {
	bank = data_packet->bank;
    start_sample_number = data_packet->starting_sample_number;
    base_addr = data_packet->fine_time - data_packet->starting_sample_number;

}

static void read_samples(const struct SW_Data_Packet* data_packet,
        hls::stream<vec_uint16_16>& inStreamSamples) {
//#pragma HLS array_partition variable=inStreamSamples factor=256
    //int ped_sample_idx = data_packet->starting_sample_number;
    //uint8_t bank = data_packet->bank;

    //changed the order: read horizontally -> read vertically
    read_samples_loop: for (int i = 0; i < NUM_SAMPLES; i++) {
        vec_uint16_16 sample;
    	read_channels_loop: for (int j = 0; j < NUM_CHANNELS; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=16
            sample[j] = data_packet->samples[i][j];
        }
        inStreamSamples << sample;

    }
}



// not using stream for peds, so 

static void ped_subtract(const uint8_t bank,
        const uint16_t start_sample_number,
        hls::stream<vec_uint16_16>& inStreamSamples,
        const vec_uint16_16 * all_peds,
        hls::stream<vec_int16_16>& outStreamPeds) {
    
    
    ped_sub_loop: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=16
        vec_uint16_16 svec = inStreamSamples.read();
        vec_int16_16 rvec;

        uint8_t idx = (s + start_sample_number) % NUM_SAMPLES;

        vec_uint16_16 pvec = all_peds[bank * NUM_SAMPLES + idx];

        // int16_t r = (int16_t)x - (int16_t)p;
        // rvec[c] = r;
        ped_sub_channel_loop: for (uint16_t c = 0; c < NUM_SAMPLES; ++c) {
            uint16_t s = svec[c];
            uint16_t p = pvec[c];
            int16_t r = (int16_t)s - (int16_t)p;
            rvec[c] = r;
        }

        outStreamPeds << rvec;
    }
}


static void integrals(const int8_t base_addr,
        hls::stream<vec_int16_16>& outStreamPeds,
        const int* bounds,
        hls::stream<vec_int16_16>& outStreamIntegral) {

    vec_int16_16 integrals[NUM_INTEGRALS];
    vec_int16_16 current;

    integral_samples_loop: for (int i = 0; i<NUM_SAMPLES; i++){
        current = outStreamPeds.read();
    	integral_bounds_loop: for (int k =0; k<NUM_INTEGRALS; k++){
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=4


            vec_int16_16 integral = (i == 0) ? 0 : integrals[k];

            int start = (bounds[2*k] + base_addr);
            if (start < 0){
                start = start + NUM_SAMPLES - 1;
            }

            int end = (bounds[2*k+1] + base_addr);
            if (end >= NUM_SAMPLES - 1){
                end = end - (NUM_SAMPLES - 1);
            }

            int linear = 0;
            if (end >= start){
                linear = 1;
            }

            int i_gte_start = (i >= start);
            int i_lte_end = (i <= end);

            if ((i_gte_start && i_lte_end) || (!linear && (i_gte_start || i_lte_end))) { //sample number is within the bound,

                integral += current;
                integrals[k] = integral; //add channel into integral array

            }
        }
    }

    for (int k =0; k<NUM_INTEGRALS; k++) {
        outStreamIntegral << integrals[k];
    }

    
}


static void write_integral_result(int32_t* output_integrals,
        hls::stream<vec_int16_16>& outStreamIntegrals) {
    
    vec_int16_16 current;
    for (int i = 0; i < NUM_INTEGRALS; i++) {
        current = outStreamIntegrals.read();
        for (int j = 0; j < NUM_CHANNELS; j++) {
                output_integrals[i * NUM_CHANNELS + j] = current[j];
        }
    }
}

void dataflow( const struct SW_Data_Packet * input_data_packet,
        const vec_uint16_16 *input_all_peds, // Read-Only Pedestals
        const int * bounds, // Read-Only Integral Bounds
        int32_t *output_integrals ,      // Output Result (Integrals)
		const uint8_t bank,
        const uint8_t start_sample_number,
        const int8_t base_addr) {
	//array of stream declaration
	static hls::stream<vec_uint16_16> inStreamSamples;
	static hls::stream<vec_int16_16> outStreamPeds;
	static hls::stream<vec_int16_16> outStreamIntegral;
	#pragma HLS STREAM variable= inStreamSamples depth=256
	#pragma HLS STREAM variable= outStreamPeds depth=256
	#pragma HLS STREAM variable= outStreamIntegral depth=64

	#pragma HLS DATAFLOW
	read_samples(input_data_packet, inStreamSamples);
	ped_subtract(bank, start_sample_number, inStreamSamples, input_all_peds, outStreamPeds);
	integrals(base_addr, outStreamPeds, bounds, outStreamIntegral);
	write_integral_result(output_integrals, outStreamIntegral);
}


extern "C" {
    void preprocess(
	        struct SW_Data_Packet * input_data_packet, // Read-Only Data Packet Struct
	        vec_uint16_16 *input_all_peds, // Read-Only Pedestals
            int * bounds, // Read-Only Integral Bounds
	        int32_t *output_integrals       // Output Result (Integrals)
	        )
    {

#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet bundle=aximm1
//#pragma HLS INTERFACE m_axi depth=8192 port=input_all_peds bundle=aximm2
//#pragma HLS INTERFACE m_axi depth=8 port=bounds bundle=aximm3
#pragma HLS INTERFACE m_axi depth=256 port=output_integrals bundle=aximm1
#pragma HLS INTERFACE mode=bram depth=512 port=input_all_peds
#pragma HLS INTERFACE mode=bram depth=8 port=bounds

        //int16_t ped_sub_results[NUM_SAMPLES][NUM_CHANNELS];
        uint8_t bank;
        uint8_t start_sample_number;
        int8_t base_addr;

        read_params(input_data_packet, bank, start_sample_number, base_addr);
        dataflow(input_data_packet, input_all_peds, bounds, output_integrals, bank, start_sample_number, base_addr);
    }
}
