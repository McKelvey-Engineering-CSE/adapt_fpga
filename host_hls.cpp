// char: 8 bit, short: 16 bit, long: 32 bit

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N
#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)

#include <vector>
#include <unistd.h>
#include <iostream>
#include <fstream>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "preprocess.h"

int data_packet_dat_to_struct(int fd, struct SW_Data_Packet * data_packet){

    // Read data into a larger buffer and then strip 
    // all of the spaces because dat files are bit-space-delineated.

    // The times two is to account for the space-delineation
    int num_bits = BUF_SIZE * 16 * 2;
    uint8_t og_buf[num_bits];

    if (read(fd, og_buf, num_bits) == -1) {
        perror("read");
    }

    for (int i = 0; i < num_bits; i++) {
        og_buf[i] = og_buf[i] - 0x30; // 0 in ASCII
    }

    uint16_t buf[BUF_SIZE];
    uint8_t ms_half, ls_half;
    int base = 0;
    for (int i = 0; i < BUF_SIZE; i++) {
        base = i * 32;
        ms_half = (og_buf[base] << 7) | (og_buf[base + 2] << 6) | (og_buf[base + 4] << 5) | (og_buf[base + 6] << 4) | (og_buf[base + 8] << 3) | (og_buf[base + 10] << 2) | (og_buf[base + 12] << 1) | og_buf[base + 14];
        ls_half = (og_buf[base + 16] << 7) | (og_buf[base + 18] << 6) | (og_buf[base + 20] << 5) | (og_buf[base + 22] << 4) | (og_buf[base + 24] << 3) | (og_buf[base + 26] << 2) | (og_buf[base + 28] << 1) | og_buf[base + 30];
        buf[i] = (ms_half << 8) | ls_half;
    }

    // First short should be 0xA1FA
    if (buf[0] != 0xa1fa) {
        printf("File must start with word 0xA1FA.\n");
        return -2;
    }

    data_packet->alpha = buf[0];
    data_packet->i2c_address = 0b111 & (buf[1] >> 13);
    data_packet->conf_address = 0b1111 & (buf[1] >> 9);
    data_packet->bank = 0b1 & (buf[1] >> 8);
    data_packet->fine_time = 0xff & buf[1];
    data_packet->coarse_time = (((uint32_t) buf[2]) << 16) & (((uint32_t) buf[3]) & 0xffff);
    data_packet->trigger_number = buf[4];
    data_packet->samples_after_trigger = (buf[5] >> 8) & 0xff;
    data_packet->look_back_samples = buf[5] & 0xff;
    data_packet->samples_to_be_read = (buf[6] >> 8) & 0xff;
    data_packet->starting_sample_number = buf[6] & 0xff;
    data_packet->number_of_missed_triggers = (buf[7] >> 8) & 0xff;
    data_packet->state_machine_status = buf[7] & 0xff;

    int buf_idx = 8;
    for (int i = 0; i < data_packet->samples_to_be_read + 1; i++) {
        for (int j = 0; j < NUM_CHANNELS; j++) {
            data_packet->samples[i][j] = buf[buf_idx] & 0xfff;
            buf_idx++;
        }
    }

    // Last short should be 0x0E6A
    if (buf[buf_idx] != 0x0e6a) {
        printf("File must end with word 0x0E6A.\n");
        return -3;
    }
    data_packet->omega = buf[buf_idx];

    return 0;
}

int peds_dat_to_arrays(int fd, vec_uint16_16 * all_peds){
    FILE * fp = fdopen(fd, "r");
    if(fp == NULL) {
        perror("fdopen");
    }

    uint16_t sample_num;
    uint16_t peds[NUM_CHANNELS];
    char line[100];
    for(int i = 0; i < NUM_SAMPLES; i++) {
        line[0] = '\0';
        if(fgets(line, sizeof(line), fp) == NULL) {
            perror("fgets");
        }
        sscanf(line, "%hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu", &sample_num, &peds[0], &peds[1], &peds[2], &peds[3], &peds[4], &peds[5], &peds[6], &peds[7], &peds[8], &peds[9], &peds[10], &peds[11], &peds[12], &peds[13], &peds[14], &peds[15]);
        for(int j = 0; j < NUM_CHANNELS; j++) {
            all_peds[0 * NUM_SAMPLES + i][j] = peds[j];
        }
    }
    for(int i = 0; i < NUM_SAMPLES; i++) {
        line[0] = '\0';
        if(fgets(line, sizeof(line), fp) == NULL) {
            perror("fgets");
        }
        sscanf(line, "%hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu", &sample_num, &peds[0], &peds[1], &peds[2], &peds[3], &peds[4], &peds[5], &peds[6], &peds[7], &peds[8], &peds[9], &peds[10], &peds[11], &peds[12], &peds[13], &peds[14], &peds[15]);
        for(int j = 0; j < NUM_CHANNELS; j++) {
            all_peds[1 * NUM_SAMPLES + i][j] = peds[j];
        }
    }
    fclose(fp);
    return 0;
}

int initialize_inputs(struct SW_Data_Packet * data_packet, vec_uint16_16 * all_peds) {
    int data_packet_fd = open("/home/warehouse/msudvarg/capstone_sp23/src/EventStream.dat", 0, "r");
    if (data_packet_fd == -1) {
        perror("open");
    }

    data_packet_dat_to_struct(data_packet_fd, data_packet);

    int peds_fd = open("/home/warehouse/msudvarg/capstone_sp23/src/peds.dat", 0, "r");
    if (peds_fd == -1) {
        perror("open");
    }

    peds_dat_to_arrays(peds_fd, all_peds);

    return 0;
}

int write_header(int fd, const char * field, uint32_t value) {
    char value_ptr[10];
    write(fd, field, strlen(field));
    write(fd, ": ", 2);
    sprintf(value_ptr, "%d", value);
    write(fd, value_ptr, strlen(value_ptr));
    write(fd, "\n", 1);
    return 0;
}

int write_integrals(int fd, const char ** bounds, int32_t *integrals) {
    char value_ptr[10];
    for (int i = 0; i < 4; i++) {
        sprintf(value_ptr, "%d (%s,%s)", i, bounds[i*2], bounds[i*2+1]);
        write(fd, value_ptr, strlen(value_ptr));
        write(fd, "   ", 3);
        for (int j = 0; j < NUM_CHANNELS; j++) {
            //sprintf(value_ptr, "%d", integrals[i][j]);
            sprintf(value_ptr, "%d", *(integrals+i*NUM_CHANNELS+j));
            write(fd, " ", 1);
            write(fd, value_ptr, strlen(value_ptr));
        }
        write(fd, "\n", 1);
    }
    return 0;
}

int write_output(int fd, const char ** bounds, int32_t *integrals, struct SW_Data_Packet * data_packet) {
    write_header(fd, "i2c_address", data_packet->i2c_address);
    write_header(fd, "conf_address", data_packet->conf_address);
    write_header(fd, "bank", data_packet->bank);
    write_header(fd, "fine_time", data_packet->fine_time);
    write_header(fd, "coarse_time", data_packet->coarse_time);
    write_header(fd, "trigger_number", data_packet->trigger_number);
    write_header(fd, "samples_after_trigger", data_packet->samples_after_trigger);
    write_header(fd, "look_back_samples", data_packet->look_back_samples);
    write_header(fd, "samples_to_be_read", data_packet->samples_to_be_read);
    write_header(fd, "starting_sample_number", data_packet->starting_sample_number);
    write_header(fd, "number_of_missed_triggers", data_packet->number_of_missed_triggers);
    write_header(fd, "state_machine_status", data_packet->state_machine_status);
    write_integrals(fd, bounds, integrals);
    return 0;
}

int produce_output(const char ** bounds, int32_t *integrals, struct SW_Data_Packet * data_packet) {
    int output_fd = open("/home/research/msudvarg/capstone_sp23/src/output.txt", O_CREAT | O_RDWR, 0666);
    if (output_fd == -1) {
        perror("open");
    }

    write_output(output_fd, bounds, integrals, data_packet);

    return 0;
}

// ------------------------------------------------------------------------------------
// Main program
// ------------------------------------------------------------------------------------
int main()
{
        printf("Beginning of main\n");
    SW_Data_Packet input_data_packet[NUM_ALPHAS];
    vec_uint16_16 input_all_peds[NUM_ALPHAS][2*NUM_SAMPLES];
    int16_t bounds[2*NUM_INTEGRALS];
    int32_t output_integrals[NUM_ALPHAS*NUM_INTEGRALS*NUM_CHANNELS];
    Centroid centroid;

    // // Initialize the data used in the test
    for (unsigned alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
        printf("Initializing inputs for alpha %u\n", alpha);
        initialize_inputs(input_data_packet + alpha,
                         input_all_peds[alpha]);
    }

    const char * bounds_strings[8] = {"-5", "5", "-10", "10", "-15", "15", "-20", "20"};
    bounds[0] = atoi(bounds_strings[0]);
    bounds[1] = atoi(bounds_strings[1]);
    bounds[2] = atoi(bounds_strings[2]);
    bounds[3] = atoi(bounds_strings[3]);
    bounds[4] = atoi(bounds_strings[4]);
    bounds[5] = atoi(bounds_strings[5]);
    bounds[6] = atoi(bounds_strings[6]);
    bounds[7] = atoi(bounds_strings[7]);

    int32_t zero_thresholds[NUM_INTEGRALS] = {-1000, -1000, -1000, 5};

    preprocess( &input_data_packet[0],
                &input_data_packet[1],
                &input_data_packet[2],
                &input_data_packet[3],
                &input_data_packet[4],
                input_all_peds,
                bounds,
                zero_thresholds,
                (int32_t *) output_integrals,
                (struct Centroid *) &centroid
                );

    int output_fd = open("/home/research/msudvarg/capstone_sp23/src/output.txt", O_CREAT | O_RDWR, 0666);
    if (output_fd == -1) {
        perror("open");
    }

    for (unsigned alpha = 0; alpha < NUM_ALPHAS; ++alpha) {
        // produce_output(bounds_strings,
        //                (int32_t *) output_integrals[alpha],
        //                (SW_Data_Packet *) &input_data_packet[alpha]);

        unsigned integral_offset = alpha * NUM_INTEGRALS * NUM_CHANNELS;
        write_output(output_fd,
                     bounds_strings,
                     output_integrals + integral_offset,
                     input_data_packet + alpha);
    }

    write_header(output_fd, "centroid_position", centroid.position);
    write_header(output_fd, "centroid_signal", centroid.signal);

    return 0;
}
