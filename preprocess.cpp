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
                  hls::stream<vec_int32_16> & ped_sub_results,
				  hls::stream<vec_int32_16> & raw_pair_data) {

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
        raw_pair_data << rvec;
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
        integral = integrals.read();
        const int32_t threshold = zero_thresholds[i];
        vec_int32_16 zeroed_integral;

        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            zeroed_integral[c] = (integral[c] < threshold) ? 0 : integral[c];
        }

        zeroed_integrals << zeroed_integral;
    }
}

void merge_integrals(hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
                     hls::stream<vec_int32_16> & merged_integrals) {
    vec_int32_16 current;
    for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
            current = zeroed_integrals[alpha].read();
            merged_integrals << current;

        }
    }
}


void island_detection(hls::stream<vec_int32_16> & merged_integrals,
                         hls::stream<vec_int32_16> & island_output,
                         hls::stream<int16_t> & stream_num_islands) {
    bool in_island_tmp;
    int16_t num_islands_tmp;
    vec_int32_16 integral;
    island_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        island_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            integral = merged_integrals.read();

            if (i == INTEGRAL_NUM) {

                island_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
                    bool in_island = (a == 0 && c == 0) ? 0 : in_island_tmp;
                    int16_t num_islands = (a == 0 && c == 0) ? 0 : num_islands_tmp;
                    if(integral[c] && !in_island) {
                        in_island = true;
                        ++num_islands;
                    }
                    else if (!integral[c] && in_island) {
                        in_island = false;
                    }
                    in_island_tmp = in_island;
                    num_islands_tmp = num_islands;
                }
            }

            island_output << integral;
        }
        if (i == INTEGRAL_NUM)
            stream_num_islands << num_islands_tmp;
    }

}

void write_islands(hls::stream<vec_int32_16> & island_output,
	             hls::stream<int16_t> & stream_num_islands,
				 vec_int32_16 output_islands[NUM_ALPHAS][NUM_INTEGRALS],
				 int16_t output_num_islands) {

    // printf("Writing islandsn");
    vec_int32_16 current;
    write_integrals_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        // printf("Pair.\n");
        write_integrals_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            current = island_output.read();
            output_islands[a][i] = current;
            // printf("Pair.\n");
        }
    }
    output_num_islands = stream_num_islands.read();
}

void write_integrals(hls::stream<vec_int32_16> & integral_output,
                     vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS]) {

    // printf("Writing integrals\n");
    vec_int32_16 current;
    write_integrals_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        // printf("Integral.\n");
        write_integrals_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            current = integral_output.read();
            output_integrals[a][i] = current;
            // printf("Integral.\n");
        }
    }
}

void write_pairs(hls::stream<vec_int32_16> pair_buffer[NUM_ALPHAS],
                     vec_int32_16 output_pairs[NUM_ALPHAS][NUM_INTEGRALS]) {
    
    // printf("Writing integrals\n");
    vec_int32_16 current;
    write_integrals_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        // printf("Integral.\n");
        write_integrals_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            current = pair_buffer[a].read();
            output_pairs[a][i] = current;
            // printf("Integral.\n");
        }
    }
}


void dataflow_alpha(const vec_uint16_16 * samples,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
        hls::stream<vec_int32_16> raw_pair_data[NUM_ALPHAS],
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
                 ped_sub_results,
				 raw_pair_data[alpha]);

	integrate(base_addrs[alpha],
              ped_sub_results,
              bounds[alpha],
              integrals);

    zero_suppress(integrals,
                  zero_thresholds[alpha],
                  zeroed_integrals[alpha]);

}


void dataflow(const SW_Data_Packet * input_data_packet0,
        const SW_Data_Packet * input_data_packet1,
        const SW_Data_Packet * input_data_packet2,
        const SW_Data_Packet * input_data_packet3,
        const SW_Data_Packet * input_data_packet4,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS],      // Output Result (Integrals)
		vec_int32_16 pair_buffer[NUM_ALPHAS][PAIR_HISTORY], // Output Pair Buffer
		vec_int32_16 output_islands[NUM_ALPHAS][NUM_INTEGRALS],
		int16_t output_num_islands,
        uint8_t banks[NUM_ALPHAS],
        uint8_t starting_sample_numbers[NUM_ALPHAS],
        int16_t base_addrs[NUM_ALPHAS]
        ) {


    hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS];
    hls::stream<vec_int32_16> raw_pair_data[NUM_ALPHAS]; // raw data stream to save pair data
    static hls::stream<vec_int32_16> merged_integrals;
    static hls::stream<vec_int32_16> island_output;
    static hls::stream<vec_int32_16> centroiding_output;
    hls::stream<int16_t> stream_num_islands;
    hls::stream<Centroid> stream_centroid;
    #pragma HLS STREAM variable=zeroed_integrals depth=4
	#pragma HLS STREAM variable=raw_pair_data depth=4
    #pragma HLS STREAM variable=merged_integrals depth=20
    #pragma HLS STREAM variable=island_output depth=20
    #pragma HLS STREAM variable=centroiding_output depth=20
    #pragma HLS STREAM variable=stream_num_islands depth=1
    #pragma HLS STREAM variable=stream_centroid depth=1

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
				raw_pair_data,
                banks,
                starting_sample_numbers,
                base_addrs,
                0);
    dataflow_alpha(input_data_packet1->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
				raw_pair_data,
                banks,
                starting_sample_numbers,
                base_addrs,
                1);
    dataflow_alpha(input_data_packet2->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
				raw_pair_data,
                banks,
                starting_sample_numbers,
                base_addrs,
                2);
    dataflow_alpha(input_data_packet3->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
				raw_pair_data,
                banks,
                starting_sample_numbers,
                base_addrs,
                3);
    dataflow_alpha(input_data_packet4->samples,
                input_all_peds,
                bounds,
                zero_thresholds,
                zeroed_integrals,
				raw_pair_data,
                banks,
                starting_sample_numbers,
                base_addrs,
                4);

    merge_integrals(zeroed_integrals,
                    merged_integrals);

    island_detection(merged_integrals,island_output,stream_num_islands);
    write_islands(island_output,stream_num_islands, output_islands, output_num_islands);
    //write_integrals(centroiding_output, output_integrals);
    write_pairs(raw_pair_data, pair_buffer);
    


}


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
#pragma HLS INTERFACE m_axi depth=1 port=output_integrals bundle=aximm6
#pragma HLS INTERFACE m_axi depth=1 port=pair_buffer bundle=aximm8
#pragma HLS INTERFACE m_axi depth=1 port=output_islands bundle=aximm9
#pragma HLS INTERFACE m_axi depth=1 port=output_num_islands bundle=aximm10


        uint8_t banks[NUM_ALPHAS];
        uint8_t starting_sample_numbers[NUM_ALPHAS];
        int16_t base_addrs[NUM_ALPHAS];

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
                        base_addrs[alpha]);
        }

        dataflow(input_data_packet0,
                 input_data_packet1,
                 input_data_packet2,
                 input_data_packet3,
                 input_data_packet4,
                 input_all_peds,
                 bounds,
                 zero_thresholds,
                 output_integrals,
				 pair_buffer,
                 output_islands,
				 output_num_islands,
                 banks,
                 starting_sample_numbers,
                 base_addrs);


    }
}
