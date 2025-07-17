#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cstring> //For memcpy
#include <hls_stream.h>
#include <iostream>
#include <iomanip>

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
        integral = integrals.read();
        const int32_t threshold = zero_thresholds[i];
        vec_int32_16 zeroed_integral;

        zero_channels: for(uint8_t c = 0; c < NUM_CHANNELS; ++c) {
            zeroed_integral[c] = (integral[c] < threshold) ? 0 : integral[c];
        }

        zeroed_integrals << zeroed_integral;
    }
}

// //random 1/0 data 
// void merge_integrals(hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS],
//                      hls::stream<vec_int32_16> & merged_integrals) {
//     vec_int32_16 current;
//     for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
//         for (uint8_t alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
//             for (uint16_t c = 0; c < NUM_CHANNELS; ++c) {
//                 current[c] = ((std::rand()&1) < 1) ? 0 : 1;
//             }
//             merged_integrals << current;
            
//         }
//     }
    
// }

// Event stream data
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

// void island_detection_2d(hls::stream<vec_int32_16> & merged_integrals,
//                          hls::stream<vec_int32_16> & island_output,
//                          hls::stream<int16_t> & stream_num_islands,
//                          hls::stream<int16_t> & stream_islands_labels) {

//         //initializing variables
//         int32_t data[ROW*COL] = {0};
//         #pragma HLS ARRAY_PARTITION variable=data cyclic factor=16 dim=1

//         int32_t label[ROW][COL] = {0};

//         vec_int32_16 integral, label_downstream;
//         uint8_t col = 0, row = 0;
//         int32_t label_tmp = 0, num_islands = 0;
//         int32_t top = 0, top_left = 0, top_right = 0, left = 0;

//         //std::cout << "Data before island_detection" << std:: endl;
//         //read in data
//         read_rows: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
//             read_cols: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
//                 integral = merged_integrals.read();

//                 if (i == INTEGRAL_NUM) {
//                     for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
// 					#pragma HLS UNROLL FACTOR=16
//                         data[a*NUM_CHANNELS + c] = integral[c];
//                         // std::cout << data[a*NUM_CHANNELS + c] << " ";
//                         // if((a*NUM_CHANNELS + c)>0 && (a*NUM_CHANNELS + c) % 10 == 9){
//                         //     std::cout << std:: endl;
//                         // }
//                     }
//                 }

//                 //output integral to downstream
//                 island_output << integral;
//             }
//         }

        
//         uint32_t mt[MERGETABLE_SIZE] = {0}, mt_reduced[MERGETABLE_SIZE] = {0};
//         #pragma HLS bind_storage variable=mt type=RAM_2P impl=bram
//         #pragma HLS bind_storage variable=mt_reduced type=RAM_2P impl=bram

//         //forward tracing
//         //assign labels to each block

//         //middle section
//         label_rows: for(uint8_t i = 0; i < ROW; i++){
//             label_cols: for(uint8_t j = 0; j < COL; j++){
//                 if(data[i*COL + j]){ //check if current blob is white

//                     // std::cout << "Data before labeling" << std:: endl;
//                     // for(uint8_t i = 0; i < ROW; i++){
//                     //     for(uint8_t j = 0; j < COL; j++){
//                     //         std::cout << label[i][j] << " ";
//                     //     }
//                     //     std::cout << std::endl;
//                     // }

//                     uint8_t assigned_flag;

//                     //research Q1: eightway_neighbor condition inside if block vs inside top_left & top_right conditions no improvement
//                     //research Q2: can we set assigned_flag in parallel as checking the neighbors(checking i,j, and labels directly for assigned_flag) no improvement
//                     //runtime vs resource
//                     #if EIGHTWAY_NEIGHBOR == 1
//                         //scan neighbors
//                         top_left = (i-1 >= 0 && j-1 >= 0 && label[i-1][j-1] > 0) ? label[i-1][j-1] : MERGETABLE_SIZE; 
//                         top = (i-1 >= 0 && label[i-1][j] > 0) ? label[i-1][j] : MERGETABLE_SIZE;
//                         top_right = (i-1 >= 0 && j+1 < COL && label[i-1][j+1] > 0) ? label[i-1][j+1] : MERGETABLE_SIZE;
//                         left = (j-1 >= 0 && label[i][j-1] > 0) ? label[i][j-1] : MERGETABLE_SIZE;

//                         assigned_flag = ((i-1 >= 0 && j-1 >= 0 && label[i-1][j-1] > 0) || (i-1 >= 0 && label[i-1][j] > 0) || 
//                                           (i-1 >= 0 && j+1 < COL && label[i-1][j+1] > 0) || (j-1 >= 0 && label[i][j-1] > 0));

//                         //update label and merge table
//                         label[i][j] = assigned_flag ? std::min(std::min(top_left, top), std::min(top_right, left)) : label_tmp + 1;
//                         mt[label[i][j]-1] = assigned_flag ? mt[label[i][j]-1] : label_tmp + 1;
//                         label_tmp = assigned_flag ? label_tmp : label_tmp + 1;

//                         //top left
//                         if (top_left!=MERGETABLE_SIZE) {
//                             mt[label[i-1][j-1]-1] = label[i][j];
//                         }
//                         //top
//                         if (top!=MERGETABLE_SIZE) {
//                             mt[label[i-1][j]-1] = label[i][j];
//                         }
//                         //top right
//                         if (top_right!=MERGETABLE_SIZE) {
//                             mt[label[i-1][j+1]-1] = label[i][j];
//                         }
//                         //left
//                         if (left!=MERGETABLE_SIZE) {
//                             mt[label[i][j-1]-1] = label[i][j];
//                         }
//                     #else
//                         //scan neighbors
//                         top = (i-1 >= 0 && j >= 0 && label[i-1][j] > 0) ? label[i-1][j] : MERGETABLE_SIZE;
//                         left = (j-1 >= 0 && label[i][j-1] > 0) ? label[i][j-1] : MERGETABLE_SIZE;
//                         assigned_flag = ((i-1 >= 0 && j >= 0 && label[i-1][j] > 0) || (j-1 >= 0 && label[i][j-1] > 0));
                        
//                         //update label and merge table
//                         label[i][j] = assigned_flag ? std::min(top,left) : label_tmp + 1;
//                         mt[label[i][j]-1] = assigned_flag ? mt[label[i][j]-1] : label_tmp + 1;
//                         label_tmp = assigned_flag ? label_tmp : label_tmp + 1;

//                         //top
//                         if (top!=MERGETABLE_SIZE) {
//                             mt[label[i-1][j]-1] = label[i][j];
//                         }

//                         //left
//                         if (left!=MERGETABLE_SIZE) {
//                             mt[label[i][j-1]-1] = label[i][j];
//                         }
//                     #endif

//                     // std::cout << "Data after labeling" << std:: endl;
//                     // for(uint8_t i = 0; i < ROW; i++){
//                     //     for(uint8_t j = 0; j < COL; j++){
//                     //         std::cout << label[i][j] << " ";
//                     //     }
//                     //     std::cout << std::endl;
//                     // }

//                 }

//             }
//             //std::cout << std::endl;
//         }

//         // std::cout << "Merge Table Before Solve" << std:: endl;
//         // for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
//         //     std::cout << mt[i] << "  ";
//         // }
//         // std::cout << std::endl;

//         //solve merge
//         for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
//         	if(mt[i]==0) break;
//             mt[i] = mt[mt[i] - 1];
//         }

//         for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
//             if(mt[i]==0) break;
//             if(mt_reduced[mt[i]-1] == 0){
//                 ++num_islands;
//                 mt_reduced[mt[i]-1] = num_islands;
//             }
//             mt[i] = mt_reduced[mt[i]-1];
//         }

//         // std::cout << "Merge Table Solved" << std:: endl;
//         // for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
//         //     std::cout << mt[i] << "  ";
//         // }
//         // std::cout << std::endl;


//         //second scan
//         //change the labels according to merge table
//         write_label_rows: for(uint8_t i = 0; i < ROW; i++){
//             write_label_cols: for(uint8_t j = 0; j < COL; j++){
//                 stream_islands_labels << ( label[i][j] ? mt[label[i][j]-1] : 0 );
//             }
//         }

//         // std::cout << "Data after island_detection" << std:: endl;
//         // for(uint8_t i = 0; i < ROW; i++){
//         //     for(uint8_t j = 0; j < COL; j++){
//         //         std::cout << (label[i][j] ? mt[label[i][j]-1] : 0) << " ";
//         //     }
//         //     std::cout << std::endl;
//         // }
        

//         //output to downstream
// 		stream_num_islands << num_islands;
//         // std::cout << "Number of islands: " << num_islands << std::endl;

// }

struct MtUpdate {
    int32_t index;
    int32_t value;
};

hls::stream<MtUpdate> stream_top;
hls::stream<MtUpdate> stream_left;
#if EIGHTWAY_NEIGHBOR == 1
hls::stream<MtUpdate> stream_top_left;
hls::stream<MtUpdate> stream_top_right;
#endif

void island_detection_2d(hls::stream<vec_int32_16> & merged_integrals,
                         hls::stream<vec_int32_16> & island_output,
                         hls::stream<int16_t> & stream_num_islands,
                         hls::stream<int16_t> & stream_islands_labels) {

        //initializing variables
        const int max_tripcount_top = MAX_UPDATES;
        const int max_tripcount_left = MERGETABLE_SIZE;
        int32_t data[ROW*COL] = {0};
        #pragma HLS ARRAY_PARTITION variable=data cyclic factor=16 dim=1

        int32_t label[ROW][COL] = {0};

        vec_int32_16 integral, label_downstream;
        int32_t label_tmp = 0, num_islands = 0;
        int32_t top = 0, top_left = 0, top_right = 0, left = 0;
        int32_t count_top = 0, count_left = 0, count_top_left = 0, count_top_right = 0;
        int32_t prev_label = 0, current_label = 0;
        MtUpdate upd;

        // std::cout << "Data before island_detection" << std:: endl;
        //read in data
        read_rows: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
            read_cols: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
                integral = merged_integrals.read();

                if (i == INTEGRAL_NUM) {
                    for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
					    #pragma HLS UNROLL FACTOR=16
                        data[a*NUM_CHANNELS + c] = integral[c];
                        // std::cout << data[a*NUM_CHANNELS + c] << " ";
                        // if((a*NUM_CHANNELS + c)>0 && (a*NUM_CHANNELS + c) % 10 == 9){
                        //     std::cout << std:: endl;
                        // }
                    }
                }

                //output integral to downstream
                island_output << integral;
            }
        }

        

        //forward tracing
        //assign labels to each block

        //middle section
        label_rows: for(uint8_t i = 0; i < ROW; i++){
            label_cols: for(uint8_t j = 0; j < COL; j++){
                #pragma HLS PIPELINE II=1
                if(data[i*COL + j]){ //check if current blob is white
                    
                    // std::cout << "Labels in first scan" << std:: endl;
                    // for(uint8_t i = 0; i < ROW; i++){
                    //     for(uint8_t j = 0; j < COL; j++){
                    //         std::cout << label[i][j] << " ";
                    //     }
                    //     std::cout << std::endl;
                    // }

                    uint8_t assigned_flag;

                    //research Q1: eightway_neighbor condition inside if block vs inside top_left & top_right conditions no improvement
                    //research Q2: can we set assigned_flag in parallel as checking the neighbors(checking i,j, and labels directly for assigned_flag) no improvement
                    //runtime vs resource
                    #if EIGHTWAY_NEIGHBOR == 1
                        //scan neighbors
                        top = (i-1 >= 0 && label[i-1][j] > 0) ? label[i-1][j] : MERGETABLE_SIZE;
                        left = (j-1 >= 0 && prev_label > 0) ? prev_label : MERGETABLE_SIZE;
                        top_left = (i-1 >= 0 && j-1 >= 0 && label[i-1][j-1] > 0) ? label[i-1][j-1] : MERGETABLE_SIZE; 
                        top_right = (i-1 >= 0 && j+1 < COL && label[i-1][j+1] > 0) ? label[i-1][j+1] : MERGETABLE_SIZE;
                        

                        assigned_flag = ((i-1 >= 0 && j-1 >= 0  && label[i-1][j-1] > 0)  || (i-1 >= 0 && label[i-1][j] > 0) || 
                                         (i-1 >= 0 && j+1 < COL && label[i-1][j+1] > 0) || (j-1 >= 0 && prev_label > 0));

                        //update label and merge table
                        current_label = assigned_flag ? std::min(std::min(top_left, top), std::min(top_right, left)) : label_tmp + 1;
                        // label[i][j] = assigned_flag ? std::min(std::min(top_left, top), std::min(top_right, left)) : label_tmp + 1;
                        // mt[label[i][j]-1] = assigned_flag ? mt[label[i][j]-1] : label_tmp + 1;
                        if (!assigned_flag) {
                            stream_top << MtUpdate{ current_label - 1, label_tmp + 1 };
                            ++count_top;
                            // std::cout << "top: " << current_label - 1 << " " << label_tmp + 1 << std::endl;
                        }
                        label_tmp = assigned_flag ? label_tmp : label_tmp + 1;

                        //top left
                        if (top_left!=MERGETABLE_SIZE) {
                            // mt[label[i-1][j-1]-1] = label[i][j];
                            stream_top_left << MtUpdate{ label[i-1][j-1] - 1, current_label };
                            ++count_top_left;
                            // std::cout << "top_left: " << label[i-1][j-1] - 1 << " " << current_label << std::endl;
                        }
                        //top
                        if (top!=MERGETABLE_SIZE) {
                            // mt[label[i-1][j]-1] = label[i][j];
                            stream_top << MtUpdate{ label[i - 1][j] - 1, current_label };
                            ++count_top;
                            // std::cout << "top: " << label[i-1][j] - 1 << " " << current_label << std::endl;
                        }
                        //top right
                        if (top_right!=MERGETABLE_SIZE) {
                            // mt[label[i-1][j+1]-1] = label[i][j];
                            stream_top_right << MtUpdate{ label[i - 1][j + 1] - 1, current_label };
                            ++count_top_right;
                            // std::cout << "top_right: " << label[i-1][j+1] - 1 << " " << current_label << std::endl;
                        }
                        //left
                        if (left!=MERGETABLE_SIZE) {
                            // mt[label[i][j-1]-1] = label[i][j];
                            stream_left << MtUpdate{ prev_label - 1, current_label };
                            ++count_left;
                            // std::cout << "left: " << prev_label - 1 << " " << current_label << std::endl;
                        }

                        prev_label = current_label;               
                        label[i][j] = current_label;
                    #else
                        //scan neighbors
                        top = (i-1 >= 0 && j >= 0 && label[i-1][j] > 0) ? label[i-1][j] : MERGETABLE_SIZE;
                        left = (j-1 >= 0 && prev_label > 0) ? prev_label : MERGETABLE_SIZE;
                        assigned_flag = ((i-1 >= 0 && j >= 0 && label[i-1][j] > 0) || (j-1 >= 0 && prev_label > 0));
                        
                        //update label and merge table
                        current_label = assigned_flag ? ((top < left) ? top : left)
                                                      : label_tmp + 1;
                        // mt[label[i][j]-1] = assigned_flag ? mt[label[i][j]-1] : label_tmp + 1;
                        if (!assigned_flag) {
                            stream_top << MtUpdate{ current_label - 1, label_tmp + 1 };
                            ++count_top;
                        }

                        label_tmp = assigned_flag ? label_tmp : label_tmp + 1;

                        

                        //top
                        if (top!=MERGETABLE_SIZE) {
                            // mt[label[i-1][j]-1] = label[i][j];
                            stream_top << MtUpdate{ label[i - 1][j] - 1, current_label };
                            ++count_top;
                        }

                        //left
                        if (left!=MERGETABLE_SIZE) {
                            // mt[label[i][j-1]-1] = label[i][j];
                            stream_left << MtUpdate{ prev_label - 1, current_label };
                            ++count_left;
                        }

                    prev_label = current_label;               
                    label[i][j] = current_label;
                        
                    #endif

                    

                    // std::cout << "Labels in first scan" << std:: endl;
                    // for(uint8_t i = 0; i < ROW; i++){
                    //     for(uint8_t j = 0; j < COL; j++){
                    //         std::cout << label[i][j] << " ";
                    //     }
                    //     std::cout << std::endl;
                    // }


                }else{
                    prev_label = 0;
                }

            }
            //std::cout << std::endl;
        }


        // std::cout << "Merge Table Before Solve" << std:: endl;
        // for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
        //     std::cout << mt[i] << "  ";
        // }
        // std::cout << std::endl;

        //solve merge
        // for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
        // 	if(mt[i]==0) break;
        //     mt[i] = mt[mt[i] - 1];
        // }

        int32_t mt[MERGETABLE_SIZE] = {0}, mt_reduced[MERGETABLE_SIZE] = {0};
        #pragma HLS bind_storage variable=mt type=RAM_2P impl=bram
        #pragma HLS bind_storage variable=mt_reduced type=RAM_2P impl=bram

        int32_t mt_pending[MERGETABLE_SIZE] = {0};
        #pragma HLS ARRAY_PARTITION variable=mt_pending complete dim=1

        
        // for (int i = 0; i < MERGETABLE_SIZE; i++) {
        //     #pragma HLS UNROLL
        //     mt_pending[i] = 0;
        // }

        // std::cout << "Mt pending before first scan" << std:: endl;
        // for (uint16_t i=0; i < MERGETABLE_SIZE; ++i) {
        //     std::cout << mt_pending[i] << "  ";
        // }
        // std::cout << std::endl;

        // Process TOP
        for (uint16_t i = 0; i < count_top; ++i) {
            #pragma HLS PIPELINE II=1
            #pragma HLS loop_tripcount min=1 max=max_tripcount_top
            upd = stream_top.read();
            mt_pending[upd.index] = (mt_pending[upd.index] == 0 || upd.value < mt_pending[upd.index])
                                ? upd.value : mt_pending[upd.index];
        }

        // Process LEFT
        for (uint16_t i = 0; i < count_left; ++i) {
            #pragma HLS PIPELINE II=1
            #pragma HLS loop_tripcount min=1 max=max_tripcount_left
            upd = stream_left.read();
            mt_pending[upd.index] = (mt_pending[upd.index] == 0 || upd.value < mt_pending[upd.index])
                                ? upd.value : mt_pending[upd.index];
        }

        #if EIGHTWAY_NEIGHBOR == 1
        // Process TOP-LEFT
        for (uint16_t i = 0; i < count_top_left; ++i) {
            #pragma HLS PIPELINE II=1
            #pragma HLS loop_tripcount min=1 max=max_tripcount_left
            upd = stream_top_left.read();
            mt_pending[upd.index] = (mt_pending[upd.index] == 0 || upd.value < mt_pending[upd.index])
                                ? upd.value : mt_pending[upd.index];
        }

        // Process TOP-RIGHT
        for (uint16_t i = 0; i < count_top_right; ++i) {
            #pragma HLS PIPELINE II=1
            #pragma HLS loop_tripcount min=1 max=max_tripcount_left
            upd = stream_top_right.read();
            mt_pending[upd.index] = (mt_pending[upd.index] == 0 || upd.value < mt_pending[upd.index])
                                ? upd.value : mt_pending[upd.index];
        }
        #endif

        // std::cout << "Mt pending" << std:: endl;
        // for (uint16_t i=0; i < MERGETABLE_SIZE; ++i) {
        //     std::cout << mt_pending[i] << "  ";
        // }
        // std::cout << std::endl;

        for (uint16_t i = 0; i < MERGETABLE_SIZE; i++) {
            if (mt_pending[i] == 0) break;
            mt[i] = mt_pending[mt_pending[i] - 1];
        }

        for (uint16_t i=0; i < MERGETABLE_SIZE; ++i) {
            if(mt[i]==0) break;
            if(mt_reduced[mt[i]-1] == 0){
                ++num_islands;
                mt_reduced[mt[i]-1] = num_islands;
            }
            mt[i] = mt_reduced[mt[i]-1];
        }

        // std::cout << "Merge Table Solved" << std:: endl;
        // for (uint8_t i=0; i < MERGETABLE_SIZE; ++i) {
        //     std::cout << mt[i] << "  ";
        // }
        // std::cout << std::endl;

        //second scan
        //change the labels according to merge table
        write_label_rows: for(uint8_t i = 0; i < ROW; i++){
            write_label_cols: for(uint8_t j = 0; j < COL; j++){
                stream_islands_labels << ( label[i][j] ? mt[label[i][j]-1] : 0 );
            }
        }

        // std::cout << "Data after island_detection" << std:: endl;
        // for(uint8_t i = 0; i < ROW; i++){
        //     for(uint8_t j = 0; j < COL; j++){
        //         std::cout << ( label[i][j] ? mt[label[i][j]-1] : 0 ) << " ";
        //     }
        //     std::cout << std::endl;
        // }
        

        //output to downstream
		stream_num_islands << num_islands;
        std::cout << "Number of islands: " << num_islands << std::endl;

}


// void centroiding(hls::stream<vec_int32_16> & merged_integrals,
//                  hls::stream<vec_int32_16> & island_output,
//                  hls::stream<vec_int32_16> & centroiding_output,
//                  hls::stream<uint8_t> & stream_num_islands,
//                  hls::stream<Centroid> & stream_centroid) {

    
//     uint16_t position_tmp;
//     uint16_t signal_tmp;  
//     vec_int32_16 integral;
//     Centroid centroid;
//     centroiding_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
//         centroiding_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
//             integral = island_output.read();

//             if (i == INTEGRAL_NUM) {

//                 centroiding_channels: for (uint8_t c = 0; c < NUM_CHANNELS; ++c) {
//                     uint16_t position = (a == 0 && c == 0) ? 0 : position_tmp;
//                     uint16_t signal = (a == 0 && c == 0) ? 0 : signal_tmp;
//                     const uint16_t pos = a * NUM_CHANNELS + c;
//                     position += pos * integral[c];
//                     signal += integral[c];
//                     position_tmp = position;
//                     signal_tmp = signal;
//                 }
//             }

//             centroiding_output << integral;
//         }
//         if (i == INTEGRAL_NUM) {
//             centroid.count = stream_num_islands.read();        
//             centroid.position = (centroid.count > 0) ? position_tmp / signal_tmp : 0;
//             centroid.signal = (centroid.count > 0) ? signal_tmp : 0;
//             stream_centroid << centroid;
//         }
//     }

// }
                

//next step look into 2d centroid
void centroiding(hls::stream<vec_int32_16> & merged_integrals,
                 hls::stream<vec_int32_16> & island_output,
                 hls::stream<int16_t> & stream_num_islands,
                 hls::stream<int16_t> & stream_islands_labels,
                 hls::stream<vec_int32_16> & centroiding_output,
                 hls::stream<Centroid> & stream_centroid) {
    uint16_t position_tmp;
    uint16_t signal_tmp;  
    vec_int32_16 integral;
    Centroid centroid;
    centroiding_integrals: for (uint8_t i = 0; i < NUM_INTEGRALS; ++i) {
        centroiding_alphas: for (uint8_t a = 0; a < NUM_ALPHAS; ++a) {
            integral = island_output.read();

            if (i == INTEGRAL_NUM) {

                read_label_rows: for(uint8_t i = 0; i < ROW; i++){
                    read_label_cols: for(uint8_t j = 0; j < COL; j++){
                        stream_islands_labels.read();
                    }
                }


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

void write_integrals(hls::stream<vec_int32_16> & centroiding_output,
                     vec_int32_16 output_integrals[NUM_ALPHAS][NUM_INTEGRALS]) {
    
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

void write_centroid(hls::stream<Centroid> & stream_centroid,
                    Centroid * centroid) {
    Centroid local_centroid;
    local_centroid = stream_centroid.read();   
    *centroid = local_centroid;
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
        struct Centroid * centroid, // Output Centroid
        uint8_t banks[NUM_ALPHAS],
        uint8_t starting_sample_numbers[NUM_ALPHAS],
        int16_t base_addrs[NUM_ALPHAS]
        ) {


    hls::stream<vec_int32_16> zeroed_integrals[NUM_ALPHAS];
    static hls::stream<vec_int32_16> merged_integrals;
    static hls::stream<vec_int32_16> island_output;
    static hls::stream<vec_int32_16> centroiding_output;
    hls::stream<int16_t> stream_num_islands;
    hls::stream<int16_t> stream_islands_labels;
    hls::stream<Centroid> stream_centroid;
    #pragma HLS STREAM variable=zeroed_integrals depth=4
    #pragma HLS STREAM variable=merged_integrals depth=20
    #pragma HLS STREAM variable=island_output depth=20
    #pragma HLS STREAM variable=centroiding_output depth=20
    #pragma HLS STREAM variable=stream_num_islands depth=1
    #pragma HLS STREAM variable=stream_islands_labels depth=1
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

    merge_integrals(zeroed_integrals,
                    merged_integrals);

    #if TWO_DIMENSION == 1
        island_detection_2d(merged_integrals,island_output,stream_num_islands,stream_islands_labels);
        centroiding(merged_integrals,island_output,stream_num_islands,stream_islands_labels,centroiding_output,stream_centroid);
    #else
        island_detection(merged_integrals,island_output,stream_num_islands);
    #endif
    
    write_integrals(centroiding_output, output_integrals);
    write_centroid(stream_centroid, centroid);
    


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
#pragma HLS INTERFACE m_axi depth=1 port=output_integrals bundle=aximm6
#pragma HLS INTERFACE m_axi depth=1 port=centroid bundle=aximm7


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
                 centroid,
                 banks,
                 starting_sample_numbers,
                 base_addrs);


    }
}
