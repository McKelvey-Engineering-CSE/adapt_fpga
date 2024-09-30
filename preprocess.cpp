#define INFINITE

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


void read_packet(hls::stream<uint16_t> & alpha_words,
                 hls::stream<Header> & packet_headers,
                 hls::stream<vec_uint16_16> & packet_samples) {

    PACKET_STATE state = STATE_START;
    Header header;
    uint8_t c;
    uint16_t sample_count;
    vec_uint16_16 sample;
    while (1) {

        uint16_t word = alpha_words.read();
        switch(state) {
            case STATE_START:
                state = (word == 0xA1FA) ? STATE_ADDR : STATE_START;
                c = 0;
                sample_count = 0;
                continue;

            case STATE_ADDR:
                header.i2c_address = 0b111 & (word >> 13);
                header.conf_address = 0b1111 & (word >> 9);
                header.bank = 0b1 & (word >> 8);
                header.fine_time = 0xff & word;
                state = STATE_TIME1;
                continue;

            case STATE_TIME1:
                header.coarse_time = (((uint32_t) word) << 16);
                state = STATE_TIME2;
                continue;

            case STATE_TIME2:
                header.coarse_time = header.coarse_time & (((uint32_t) word) & 0xffff);
                state = STATE_TRIGGERNUM;
                continue;

            case STATE_TRIGGERNUM:
                header.trigger_number = word;
                state = STATE_SAMPLESAFTERTRIG;
                continue;

            case STATE_SAMPLESAFTERTRIG:
                header.samples_after_trigger = (word >> 8) & 0xff;
                header.look_back_samples = word & 0xff;
                state = STATE_SAMPLESTOREAD;
                continue;

            case STATE_SAMPLESTOREAD:
                header.samples_to_be_read = (word >> 8) & 0xff;
                header.starting_sample_number = word & 0xff;
                state = STATE_MISSEDTRIGGERS;
                continue;

            case STATE_MISSEDTRIGGERS:
                header.number_of_missed_triggers = (word >> 8) & 0xff;
                header.state_machine_status = word & 0xff;
                state = STATE_READING;
                continue;

            case STATE_READING:
                sample[c] = word & 0xfff; // 1024
                if (c == NUM_CHANNELS - 1) {
                    c = 0;
                    packet_samples << sample;
                }
                else {
                    ++c;
                }
                if (sample_count == NUM_CHANNELS * NUM_SAMPLES - 1) {
                    sample_count = 0;
                    packet_headers << header;
                    state = STATE_END;
                }
                else {
                    ++sample_count;
                }
                continue;

            case STATE_END:
                state = (word == 0X0E6A) ? STATE_START : STATE_END;
                #ifdef INFINITE
                continue;
                #endif
                break;

        }
    }

}


void ped_subtract(hls::stream<Header> & headers_in,
                  hls::stream<Header> & headers_out,
                  hls::stream<vec_uint16_16> & samples_in,
                  hls::stream<vec_int32_16> & ped_sub_results,
                  const vec_uint16_16 * peds) {

    
    #ifdef INFINITE
    while (1) {
        Header header = headers_in.read();
        headers_out << header;

        ped_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
            vec_uint16_16 svec = samples_in.read();

            // calculate base address for integral 
            const uint16_t idx = (header.starting_sample_number + s) % NUM_SAMPLES;
            vec_uint16_16 pvec = peds[header.bank * NUM_SAMPLES + idx];

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
    #endif
    // Some as above without while(1)
    Header header = headers_in.read();
        headers_out << header;

        ped_samples: for (uint16_t s = 0; s < NUM_SAMPLES; ++s) {
            vec_uint16_16 svec = samples_in.read();

            // calculate base address for integral 
            const uint16_t idx = (header.starting_sample_number + s) % NUM_SAMPLES;
            vec_uint16_16 pvec = peds[header.bank * NUM_SAMPLES + idx];

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

void integrate(hls::stream<Header> & headers_in,
               hls::stream<vec_int32_16> & ped_sub_results,
               const int16_t *bounds,
               hls::stream<vec_int32_16> & integrals) {
    #ifdef INFINITE
    while (1) {
    #endif
        Header header = headers_in.read();

        uint16_t base_addr = header.fine_time - header.starting_sample_number;
        base_addr = (base_addr < 0) ? base_addr + NUM_SAMPLES : base_addr;

        vec_int32_16 samples;
        vec_int32_16 tmp_integrals[NUM_INTEGRALS];

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
}

void zero_suppress(hls::stream<vec_int32_16> & integrals,
                   const int32_t * zero_thresholds,
                   hls::stream<vec_int32_16> & zeroed_integrals) {

    #ifdef INFINITE
    while (1) {
    #endif

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
}

void merge_integrals(hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
                     hls::stream<vec_int32_16> & merged_integrals) {

    #ifdef INFINITE
    while (1) {
    #endif
        vec_int32_16 current;
        for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
                current = zeroed_integrals[alpha].read();
                merged_integrals << current;

            }
        }
    }
}


void island_detection(hls::stream<vec_int32_16> & merged_integrals,
                         hls::stream<vec_int32_16> & island_output,
                         hls::stream<int16_t> & stream_num_islands) {

    #ifdef INFINITE
    while (1) {
    #endif
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

}

void centroiding(hls::stream<vec_int32_16> & island_output,
                 hls::stream<int16_t> & stream_num_islands,
                 hls::stream<vec_int32_16> & centroiding_output,
                 hls::stream<Centroid> & stream_centroid) {
    
    #ifdef INFINITE
    while (1) {
    #endif
        uint16_t position_tmp;
        uint16_t signal_tmp;  
        vec_int32_16 integral;
        Centroid centroid;
        centroiding_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            centroiding_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
                integral = island_output.read();

                if (i == INTEGRAL_NUM) {

                    centroiding_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
                        uint16_t position = (a == 0 && c == 0) ? 0 : position_tmp;
                        uint16_t signal = (a == 0 && c == 0) ? 0 : signal_tmp;
                        const uint16_t pos = a * NUM_CHANNELS + c;
                        position += pos * integral[c];
                        signal += integral[c];
                        position_tmp = position;
                        signal_tmp = signal;
                    }
                }

                centroiding_output << integral;
            }
            if (i == INTEGRAL_NUM) {
                centroid.count = stream_num_islands.read();        
                centroid.position = (centroid.count > 0) ? position_tmp / signal_tmp : 0;
                centroid.signal = (centroid.count > 0) ? signal_tmp : 0;
                stream_centroid << centroid;
            }
        }
    }
}

void write_integrals(hls::stream<vec_int32_16> & centroiding_output,
                     vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS]) {
    
    #ifdef INFINITE
    while (1) {
    #endif
        // printf("Writing integrals\n");
        vec_int32_16 current;
        write_integrals_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            // printf("Integral.\n");
            write_integrals_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
                current = centroiding_output.read();
                output_integrals[a][i] = current;
                // printf("Integral.\n");
            }
        }
    }
}

void write_centroid(hls::stream<Centroid> & stream_centroid,
                    Centroid * centroid) {
    #ifdef INFINITE
    while (1) {
    #endif
        Centroid local_centroid;
        local_centroid = stream_centroid.read();   
        *centroid = local_centroid;
    }
}

void dataflow_alpha(hls::stream<uint16_t> & input_alpha,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
        const uint8_t alpha
        ) {

    #pragma HLS FUNCTION_INSTANTIATE variable=alpha

    hls::stream<Header> headers_ped;
    hls::stream<Header> headers_integrate;
	hls::stream<vec_uint16_16> packet_samples;
	hls::stream<vec_int32_16> ped_sub_results;
	hls::stream<vec_int32_16> integrals;
	#pragma HLS STREAM variable=headers_ped depth=1
	#pragma HLS STREAM variable=headers_integrate depth=1
	#pragma HLS STREAM variable=packet_samples depth=256
	#pragma HLS STREAM variable=ped_sub_results depth=256
	#pragma HLS STREAM variable=integrals depth=4

	#pragma HLS DATAFLOW

    read_packet(input_alpha, headers_ped, packet_samples);

	ped_subtract(headers_ped,
                 headers_integrate,
                 packet_samples,
                 ped_sub_results,                 
                 input_all_peds[alpha]);

	integrate(headers_integrate,
              ped_sub_results,
              bounds[alpha],
              integrals);

    zero_suppress(integrals,
                  zero_thresholds[alpha],
                  zeroed_integrals[alpha]);

}

void dataflow(hls::stream<uint16_t> & input_alpha0,
              hls::stream<uint16_t> & input_alpha1,
              hls::stream<uint16_t> & input_alpha2,
              hls::stream<uint16_t> & input_alpha3,
              hls::stream<uint16_t> & input_alpha4,
        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
        const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
        const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
        vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS],      // Output Result (Integrals)
        struct Centroid * centroid // Output Centroid
        ) {


    hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS];
    static hls::stream<vec_int32_16> merged_integrals;
    static hls::stream<vec_int32_16> island_output;
    static hls::stream<vec_int32_16> centroiding_output;
    hls::stream<int16_t> stream_num_islands;
    hls::stream<Centroid> stream_centroid;
    #pragma HLS STREAM variable=zeroed_integrals depth=4
    #pragma HLS STREAM variable=merged_integrals depth=20
    #pragma HLS STREAM variable=island_output depth=20
    #pragma HLS STREAM variable=centroiding_output depth=20
    #pragma HLS STREAM variable=stream_num_islands depth=1
    #pragma HLS STREAM variable=stream_centroid depth=1

	#pragma HLS DATAFLOW

    #pragma HLS array_partition variable=input_all_peds type=complete dim=1
    #pragma HLS array_partition variable=bounds type=complete dim=1
    #pragma HLS array_partition variable=zero_thresholds type=complete dim=1

    DATAFLOW_ALPHA(0);
    DATAFLOW_ALPHA(1);
    DATAFLOW_ALPHA(2);
    DATAFLOW_ALPHA(3);
    DATAFLOW_ALPHA(4);

    merge_integrals(zeroed_integrals,
                    merged_integrals);

    island_detection(merged_integrals,island_output,stream_num_islands);
    centroiding(island_output,stream_num_islands,centroiding_output,stream_centroid);
    write_integrals(centroiding_output, output_integrals);
    write_centroid(stream_centroid, centroid);
    
}


extern "C" {
    void preprocess(
	        hls::stream<uint16_t> & input_alpha0,
            hls::stream<uint16_t> & input_alpha1,
            hls::stream<uint16_t> & input_alpha2,
            hls::stream<uint16_t> & input_alpha3,
            hls::stream<uint16_t> & input_alpha4,
	        const vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES], // Read-Only Pedestals
            const int16_t bounds[NUM_ALPHAS][2*NUM_INTEGRALS], // Read-Only Integral Bounds
            const int32_t zero_thresholds[NUM_ALPHAS][NUM_INTEGRALS], // Read-Only Thresholds for zero-suppression
	        vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS],       // Output Result (Integrals)
            struct Centroid *centroid // Output Centroid
	        )
    {
#pragma HLS INTERFACE axis depth=1 port=input_alpha0
#pragma HLS INTERFACE axis depth=1 port=input_alpha1
#pragma HLS INTERFACE axis depth=1 port=input_alpha2
#pragma HLS INTERFACE axis depth=1 port=input_alpha3
#pragma HLS INTERFACE axis depth=1 port=input_alpha4
#pragma HLS INTERFACE mode=bram depth=1 port=input_all_peds
// #pragma HLS array_partition variable=input_all_peds type=complete dim=1
#pragma HLS INTERFACE mode=bram depth=1 port=bounds
#pragma HLS INTERFACE mode=bram depth=4 port=zero_thresholds
#pragma HLS INTERFACE m_axi depth=1 port=output_integrals bundle=aximm1
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm2


        dataflow(input_alpha0,
                 input_alpha1,
                 input_alpha2,
                 input_alpha3,
                 input_alpha4,
                 input_all_peds,
                 bounds,
                 zero_thresholds,
                 output_integrals,
                 centroid);


    }
}
