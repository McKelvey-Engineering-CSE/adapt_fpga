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
                 int16_t &base_addr,
                 const int16_t *bounds,
                 int16_t *local_bounds,
                 const int32_t *zero_thresholds,
                 int32_t *local_zero_thresholds) {
	bank = data_packet->bank;
    starting_sample_number = data_packet->starting_sample_number;
    base_addr = data_packet->fine_time - data_packet->starting_sample_number;
    base_addr = (base_addr < 0) ? base_addr + NUM_SAMPLES : base_addr;
    memcpy(local_bounds,bounds,2*NUM_INTEGRALS*sizeof(int16_t));
    memcpy(local_zero_thresholds,zero_thresholds,NUM_INTEGRALS*sizeof(int32_t));
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
        
        ped_channel: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
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
                   const int32_t * zero_thresholds,
                   hls::stream<vec_int32_16> & zeroed_integrals) {

    vec_int32_16 integral;

    zero_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        #pragma HLS UNROLL factor=4
        integral = integrals.read();
        const int32_t threshold = zero_thresholds[i];
        vec_int32_16 zeroed_integral;

        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            zeroed_integral[c] = (integral[c] < threshold) ? 0 : integral[c];
        }

        zeroed_integrals << zeroed_integral;
    }
}

void copy_outputs(hls::stream<vec_int32_16> & zeroed_integrals, const uint8_t alpha) {
    vec_int32_16 integral;

    copy_integrals: for(uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        integral = zeroed_integrals.read();
        // zeroed_integrals_global[alpha][i] = integral;
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

        centroiding_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
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

void write_outputs_dataflow(hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
                            int32_t * output_integrals,
                            const uint8_t integral_num,
                            hls::stream<vec_int32_16> & for_islands) {
    vec_int32_16 current;
    for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
            current = zeroed_integrals[alpha].read();
            if(i == integral_num) {
                // for_islands << current;
            }
            for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
                output_integrals[alpha * NUM_INTEGRALS * NUM_CHANNELS +
                                i * NUM_CHANNELS + c] = current[c];
            }

        }
    }
}

void write_outputs(Centroid * centroid, int32_t * output_integrals) {
    *centroid = local_centroid;
    memcpy(output_integrals, zeroed_integrals_global, NUM_ALPHAS*NUM_INTEGRALS*NUM_CHANNELS*sizeof(int32_t));
}

void dataflow_alpha(const vec_uint16_16 * samples,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
		const uint8_t banks[NUM_ALPHAS],
        const uint8_t starting_sample_numbers[NUM_ALPHAS],
        const int16_t base_addrs[NUM_ALPHAS],
        const uint8_t alpha
        ) {

    #pragma HLS FUNCTION_INSTANTIATE variable=alpha

	hls::stream<vec_uint16_16> packet_samples;
	hls::stream<vec_int32_16> ped_sub_results;
	hls::stream<vec_int32_16> integrals;
	#pragma HLS STREAM variable=packet_samples depth=256
	#pragma HLS STREAM variable=ped_sub_results depth=256
	#pragma HLS STREAM variable=integrals depth=4

	#pragma HLS DATAFLOW

	read_samples(samples,
                 packet_samples);

	ped_subtract(banks[alpha],
                 starting_sample_numbers[alpha],
                 packet_samples,
                 input_all_peds[alpha],
                 ped_sub_results);

	integrate(base_addrs[alpha],
              ped_sub_results,
              bounds[alpha],
              integrals);

    zero_suppress(integrals,
                  zero_thresholds[alpha],
                  zeroed_integrals[alpha]);

    // copy_outputs(zeroed_integrals[alpha], alpha);
}


void dataflow(const SW_Data_Packet * input_data_packet0,
        const SW_Data_Packet * input_data_packet1,
        const SW_Data_Packet * input_data_packet2,
        const SW_Data_Packet * input_data_packet3,
        const SW_Data_Packet * input_data_packet4,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        int32_t * output_integrals,      // Output Result (Integrals)
        struct Centroid * centroid, // Output Centroid
        uint8_t banks[NUM_ALPHAS],
        uint8_t starting_sample_numbers[NUM_ALPHAS],
        int16_t base_addrs[NUM_ALPHAS]
        ) {


    hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS];
    static hls::stream<vec_int32_16> for_islands;
    #pragma HLS STREAM variable=zeroed_integrals depth=4
    #pragma HLS STREAM variable=zeroed_integrals depth=5

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

	#pragma HLS DATAFLOW

    #pragma HLS array_partition variable=input_all_peds type=complete dim=1
    #pragma HLS array_partition variable=banks type=complete dim=1
    #pragma HLS array_partition variable=starting_sample_numbers type=complete dim=1
    #pragma HLS array_partition variable=base_addrs type=complete dim=1
    #pragma HLS array_partition variable=bounds type=complete dim=1
    #pragma HLS array_partition variable=zero_thresholds type=complete dim=1

    dataflow_alpha(input_data_packet0->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
                banks,
                starting_sample_numbers,
                base_addrs,
                0);
    dataflow_alpha(input_data_packet1->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
                banks,
                starting_sample_numbers,
                base_addrs,
                1);
    dataflow_alpha(input_data_packet2->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
                banks,
                starting_sample_numbers,
                base_addrs,
                2);
    dataflow_alpha(input_data_packet3->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
                banks,
                starting_sample_numbers,
                base_addrs,
                3);
    dataflow_alpha(input_data_packet4->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
                banks,
                starting_sample_numbers,
                base_addrs,
                4);

    write_outputs_dataflow(zeroed_integrals,
                                output_integrals,
                                3,
                                for_islands);

    // island_detection(3, zeroed_integrals[]) {
    //     bool in_island = 0;
    //     int16_t num_islands = 0;

    // }
    


}


extern "C" {
    void preprocess(
	        const struct SW_Data_Packet * input_data_packet0, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet1, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet2, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet3, // Read-Only Data Packet Struct
	        const struct SW_Data_Packet * input_data_packet4, // Read-Only Data Packet Struct
	        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
            const int16_t bounds[2*NUM_INTEGRALS], // Read-Only Integral Bounds
            const int32_t zero_thresholds[NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
	        int32_t *output_integrals,       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet0 bundle=aximm1
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet1 bundle=aximm2
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet2 bundle=aximm3
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet3 bundle=aximm4
#pragma HLS INTERFACE m_axi depth=1 port=input_data_packet4 bundle=aximm5
#pragma HLS INTERFACE mode=bram depth=1 port=input_all_peds
// #pragma HLS array_partition variable=input_all_peds type=complete dim=1
#pragma HLS INTERFACE mode=bram depth=1 port=bounds
#pragma HLS INTERFACE mode=bram depth=4 port=zero_thresholds
#pragma HLS INTERFACE m_axi depth=320 port=output_integrals bundle=aximm6
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm7


        uint8_t banks[NUM_ALPHAS];
        uint8_t starting_sample_numbers[NUM_ALPHAS];
        int16_t base_addrs[NUM_ALPHAS];
        int16_t local_bounds[NUM_ALPHAS][2*NUM_INTEGRALS];
        int32_t local_zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS];

        loop_alphas: for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
            const SW_Data_Packet * input_data_packet;

            switch (alpha) {
                case 0: input_data_packet = input_data_packet0; break;
                case 1: input_data_packet = input_data_packet1; break;
                case 2: input_data_packet = input_data_packet2; break;
                case 3: input_data_packet = input_data_packet3; break;
                case 4: input_data_packet = input_data_packet4; break;
            }

            read_params(input_data_packet,
                        banks[alpha],
                        starting_sample_numbers[alpha],
                        base_addrs[alpha],
                        bounds,
                        local_bounds[alpha],
                        zero_thresholds,
                        local_zero_thresholds[alpha]);
        }

        dataflow(input_data_packet0,
                 input_data_packet1,
                 input_data_packet2,
                 input_data_packet3,
                 input_data_packet4,
                 input_all_peds,
                 local_bounds,
                 local_zero_thresholds,
                 output_integrals,
                 centroid,
                 banks,
                 starting_sample_numbers,
                 base_addrs);



        // centroiding(&local_centroid, 3);

        // write_outputs(centroid, output_integrals);

    }
}
