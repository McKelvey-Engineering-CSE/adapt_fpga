#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cstring> //For memcpy
#include <hls_stream.h>

// char: 8 bit, short: 16 bit, long: 32 bit
#include "preprocess.h"


vec_int32_16 zeroed_integrals_global[NUM_ALPHAS][NUM_INTEGRALS];
Centroid local_centroid;

void read_params(const SW_Data_Packet* data_packet,
                 uint8_t &bank,
                 uint8_t &starting_sample_number,
                 int16_t &base_addr) {
	bank = data_packet->bank;
    starting_sample_number = data_packet->starting_sample_number;
    base_addr = data_packet->fine_time - data_packet->starting_sample_number;
    base_addr = (base_addr < 0) ? base_addr + NUM_SAMPLES : base_addr;
}

static void read_samples(const vec_uint16_16 * samples,
                         hls::stream<vec_uint16_16>& packet_samples) {
    read_samples_loop: for (int i = 0; i < NUM_SAMPLES; i++) {
        vec_uint16_16 sample = samples[i];
        packet_samples << sample;
    }
}


void ped_subtract(const uint8_t bank,
                  const uint8_t starting_sample_number,
                  hls::stream<vec_uint16_16> & packet_samples,
                  const vec_uint16_16 * peds,
                  hls::stream<vec_int32_16> & ped_sub_results) {

    ped_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {

        vec_uint16_16 svec = packet_samples.read();

        // calculate base address for integral        
        const uint16_t idx = (starting_sample_number + s) % NUM_SAMPLES;        
        vec_uint16_16 pvec = peds[bank * NUM_SAMPLES + idx];
        
        vec_int32_16 rvec;
        
        ped_channel: for (uint16_t c = 0; c < NUM_CHANNELS; ++c) {
            uint16_t s = svec[c];
            uint16_t p = pvec[c];
            int32_t r = (int32_t) s - (int32_t) p;
            rvec[c] = r;
        }
        ped_sub_results << rvec;
    }
}

void integrate(const int16_t base_addr,
              hls::stream<vec_int32_16> & ped_sub_results,
              const int16_t *bounds,
              hls::stream<vec_int32_16> & integrals) {

    vec_int32_16 samples;
    vec_int32_16 tmp_integrals[NUM_INTEGRALS];
    // #pragma HLS ARRAY_PARTITION variable=tmp_integrals type=complete dim=1

    int_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {

        samples = ped_sub_results.read();

        int_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            #pragma HLS UNROLL factor=4
            const int16_t start = bounds[2*i];
            const int16_t end = bounds[2*i+1];
            vec_int32_16 current_integral = (s == 0) ? 0 : tmp_integrals[i];
        
            const int16_t x = s - base_addr;
            current_integral =
                ((x >= start && x <= end) || (x - NUM_SAMPLES) >= start) ?
                current_integral + samples :
                current_integral;
            tmp_integrals[i] = current_integral;

        }
    }

    for (int i = 0; i < NUM_INTEGRALS; ++i) {
        integrals << tmp_integrals[i];
    }
}

void zero_suppress(hls::stream<vec_int32_16> & integrals,
                   const int32_t * zero_thesholds,
                   hls::stream<vec_int32_16> & zeroed_integrals) {

    vec_int32_16 integral;

    zero_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        #pragma HLS UNROLL factor=4
        integral = integrals.read();
        const int32_t threshold = zero_thesholds[i];
        vec_int32_16 zeroed_integral;

        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            zeroed_integral[c] = (integral[c] < threshold) ? 0 : integral[c];
        }

        zeroed_integrals << zeroed_integral;
    }
}

int16_t island_detection(const uint8_t integral_num) {
    bool in_island = 0;
    int16_t num_islands = 0;
    island_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
        #pragma HLS UNROLL factor=5

        const vec_int32_16 zvec = zeroed_integrals_global[a][integral_num];

        island_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            #pragma HLS UNROLL factor=16
            if(zvec[c] && !in_island) {
                in_island = true;
                ++num_islands;
            }
            else if (!zvec[c] && in_island) {
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

        const vec_int32_16 zvec = zeroed_integrals_global[a][integral_num];

        centroiding_channels: for (unsigned c = 0; c < NUM_CHANNELS; ++c) {
            #pragma HLS UNROLL factor=16
            const uint16_t pos = a * NUM_CHANNELS + c;
            position += pos * zvec[c];
            signal += zvec[c];
        }
    }
    centroid->position = (count > 0) ? position / signal : 0;
    centroid->signal = (count > 0) ? signal : 0;
    return count;
}

void write_outputs(Centroid * centroid, int32_t * output_integrals) {
    *centroid = local_centroid;
    memcpy(output_integrals, zeroed_integrals_global, NUM_ALPHAS*NUM_INTEGRALS*NUM_CHANNELS*sizeof(int32_t));
}

void dataflow_alpha(const vec_uint16_16 * samples,
        const vec_uint16_16 *peds, // Read-Only Pedestals
        const int16_t bounds[2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t *zero_thresholds, // Read-Only Thresholds for zero-suppression
        int32_t *output_integrals,      // Output Result (Integrals)
        struct Centroid *centroid, // Output Centroid
		const uint8_t bank,
        const uint8_t starting_sample_number,
        const int16_t base_addr,
        hls::stream<vec_int32_16> & zeroed_integrals
        ) {

	static hls::stream<vec_uint16_16> packet_samples;
	static hls::stream<vec_int32_16> ped_sub_results;
	static hls::stream<vec_int32_16> integrals;
	#pragma HLS STREAM variable=packet_samples depth=256
	#pragma HLS STREAM variable=ped_sub_results depth=256
	#pragma HLS STREAM variable=integrals depth=4

	#pragma HLS DATAFLOW
	read_samples(samples,
                 packet_samples);

	ped_subtract(bank,
                 starting_sample_number,
                 packet_samples,
                 peds,
                 ped_sub_results);

	integrate(base_addr,
              ped_sub_results,
              bounds,
              integrals);

    zero_suppress(integrals,
                  zero_thresholds,
                  zeroed_integrals);
}

void copy_outputs(hls::stream<vec_int32_16> & zeroed_integrals, const uint8_t alpha) {
    vec_int32_16 integral;

    copy_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        integral = zeroed_integrals.read();
        zeroed_integrals_global[alpha][i] = integral;
    }
}

void dataflow(const SW_Data_Packet * input_data_packet,
        const vec_uint16_16 * input_all_peds, // Read-Only Pedestals
        const int16_t * bounds, // Read-Only Integral Bounds
        const int32_t * zero_thresholds, // Read-Only Thresholds for zero-suppression
        int32_t * output_integrals,      // Output Result (Integrals)
        struct Centroid * centroid // Output Centroid
        ) {

    static hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS];
	#pragma HLS STREAM variable=zeroed_integrals depth=4

    // static hls::stream<vec_int32_16> zeroed_integrals_0;
	// #pragma HLS STREAM variable=zeroed_integrals_0 depth=4
    // static hls::stream<vec_int32_16> zeroed_integrals_1;
	// #pragma HLS STREAM variable=zeroed_integrals_1 depth=4
    // static hls::stream<vec_int32_16> zeroed_integrals_2;
	// #pragma HLS STREAM variable=zeroed_integrals_2 depth=4
    // static hls::stream<vec_int32_16> zeroed_integrals_3;
	// #pragma HLS STREAM variable=zeroed_integrals_3 depth=4
    // static hls::stream<vec_int32_16> zeroed_integrals_4;
	// #pragma HLS STREAM variable=zeroed_integrals_4 depth=4
    
    // static hls::stream<vec_int32_16> * zeroed_integrals[NUM_ALPHAS] = 
    // {
    //     &zeroed_integrals_0, &zeroed_integrals_1,
    //     &zeroed_integrals_2, &zeroed_integrals_3,
    //     &zeroed_integrals_4
    // };

	//#pragma HLS DATAFLOW

    loop_alphas: for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {

        uint8_t bank;
        uint8_t starting_sample_number;
        int16_t base_addr;

        read_params(input_data_packet + alpha,
                    bank,
                    starting_sample_number,
                    base_addr);

        dataflow_alpha(input_data_packet[alpha].samples,
                    input_all_peds + alpha * 2 * NUM_SAMPLES,
                    bounds,
                    zero_thresholds,
                    output_integrals,
                    centroid,
                    bank,
                    starting_sample_number,
                    base_addr,
                    zeroed_integrals[alpha]);

        copy_outputs(zeroed_integrals[alpha], alpha);
    }


}

extern "C" {
    void preprocess(
	        const struct SW_Data_Packet input_data_packet[NUM_ALPHAS], // Read-Only Data Packet Struct
	        const vec_uint16_16 *input_all_peds, // Read-Only Pedestals
            const int16_t bounds[2*NUM_INTEGRALS], // Read-Only Integral Bounds
            const int32_t *zero_thresholds, // Read-Only Thresholds for zero-suppression
	        int32_t *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet bundle=aximm1
#pragma HLS INTERFACE mode=bram depth=2560 port=input_all_peds
#pragma HLS INTERFACE mode=bram depth=1 port=bounds
#pragma HLS array_partition variable=bounds type=block factor=4
#pragma HLS INTERFACE mode=bram depth=4 port=zero_thresholds
#pragma HLS INTERFACE m_axi depth=320 port=output_integrals bundle=aximm5
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm6

        dataflow(input_data_packet,
                 input_all_peds,
                 bounds,
                 zero_thresholds,
                 output_integrals,
                 centroid);


        centroiding(&local_centroid, 3);

        write_outputs(centroid, output_integrals);

    }
}
