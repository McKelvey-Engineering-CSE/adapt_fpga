/**********
Copyright (c) 2018, Xilinx, Inc.
All rights reserved.
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********/

#define CL_HPP_CL_1_2_DEFAULT_BUILD
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_ENABLE_PROGRAM_CONSTRUCTION_FROM_ARRAY_COMPATIBILITY 1
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS

#define DATA_SIZE 4096

// char: 8 bit, short: 16 bit, long: 32 bit

#define NUM_CHANNELS 16
#define NUM_SAMPLES 256 // N
#define BUF_SIZE 4105 // 8 + N*16 + 1 words (16 bits / 2 bytes per word)


#include <vector>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <CL/cl2.hpp>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>


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

int data_packet_dat_to_struct(int fd, SW_Data_Packet * data_packet){

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

int peds_dat_to_arrays(int fd, uint16_t * all_peds){
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
            all_peds[0*(NUM_SAMPLES*NUM_CHANNELS) + i*NUM_CHANNELS + j] = peds[j];
        }
    }
    for(int i = 0; i < NUM_SAMPLES; i++) {
        line[0] = '\0';
        if(fgets(line, sizeof(line), fp) == NULL) {
            perror("fgets");
        }
        sscanf(line, "%hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu %hu", &sample_num, &peds[0], &peds[1], &peds[2], &peds[3], &peds[4], &peds[5], &peds[6], &peds[7], &peds[8], &peds[9], &peds[10], &peds[11], &peds[12], &peds[13], &peds[14], &peds[15]);
        for(int j = 0; j < NUM_CHANNELS; j++) {
            all_peds[1*(NUM_SAMPLES*NUM_CHANNELS) + i*NUM_CHANNELS + j] = peds[j];
        }
    }
    fclose(fp);
    return 0;
}

int initialize_inputs(SW_Data_Packet * data_packet, uint16_t * all_peds) {
    int data_packet_fd = open("../../src/EventStream.dat", 0, "r");
    if (data_packet_fd == -1) {
        perror("open");
    }

    data_packet_dat_to_struct(data_packet_fd, data_packet);

    int peds_fd = open("../../src/peds.dat", 0, "r");
    if (peds_fd == -1) {
        perror("open");
    }

    peds_dat_to_arrays(peds_fd, all_peds);

    return 0;
}

int write_header(int fd, char * field, uint32_t value) {
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

int write_output(int fd, const char ** bounds, int32_t *integrals, SW_Data_Packet * data_packet) {
    
    // char str_ptr[100];
    // sprintf(str_ptr,"i2c_address");
    char* str_ptr = (char *)"i2c_address";

    write_header(fd, str_ptr, data_packet->i2c_address);
    
    str_ptr = (char *)"conf_address";

    write_header(fd, str_ptr, data_packet->conf_address);
    
    str_ptr = (char *)"bank";

    write_header(fd, str_ptr, data_packet->bank);
    str_ptr = (char *)"fine_time";
    write_header(fd, str_ptr, data_packet->fine_time);
    str_ptr = (char *)"coarse_time";
    write_header(fd, str_ptr, data_packet->coarse_time);
    str_ptr = (char *)"trigger_number";
    write_header(fd, str_ptr, data_packet->trigger_number);
    str_ptr = (char *)"samples_after_trigger";
    write_header(fd, str_ptr, data_packet->samples_after_trigger);
    str_ptr = (char *)"look_back_samples";
    write_header(fd, str_ptr, data_packet->look_back_samples);
    str_ptr = (char *)"samples_to_be_read";
    write_header(fd, str_ptr, data_packet->samples_to_be_read);
    str_ptr = (char *)"starting_sample_number";
    write_header(fd, str_ptr, data_packet->starting_sample_number);
    str_ptr = (char *)"number_of_missed_triggers";
    write_header(fd, str_ptr, data_packet->number_of_missed_triggers);
    str_ptr = (char *)"state_machine_status";
    write_header(fd, str_ptr, data_packet->state_machine_status);
    write_integrals(fd, bounds, integrals);
    return 0;
}

int produce_output(const char ** bounds, int32_t *integrals, SW_Data_Packet * data_packet) {
    int output_fd = open("../../src/output.txt", O_CREAT | O_RDWR, 0666);
    if (output_fd == -1) {
        perror("open");
    }

    write_output(output_fd, bounds, integrals, data_packet);
    return 0; // ?? should just change to void 
}

// Forward declaration of utility functions included at the end of this file
std::vector<cl::Device> get_xilinx_devices();
char *read_binary_file(const std::string &xclbin_file_name, unsigned &nb);

// ------------------------------------------------------------------------------------
// Main program
// ------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // ------------------------------------------------------------------------------------
    // Step 1: Initialize the OpenCL environment
    // ------------------------------------------------------------------------------------
    cl_int err;
    std::string binaryFile = (argc != 2) ? "preprocess.xclbin" : argv[1]; // COMPILED BINARY
    unsigned fileBufSize;
    std::vector<cl::Device> devices = get_xilinx_devices();
    devices.resize(1);
    cl::Device device = devices[0];
    cl::Context context(device, NULL, NULL, NULL, &err);
    char *fileBuf = read_binary_file(binaryFile, fileBufSize);
    cl::Program::Binaries bins{{fileBuf, fileBufSize}};
    cl::Program program(context, devices, bins, NULL, &err);
    cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE, &err);
    cl::Kernel krnl_preprocess(program, "preprocess", &err); // HW FUNCTION NAME
    // cl::Kernel krnl_new_name(program, "new_name", &err);

    // ------------------------------------------------------------------------------------
    // Step 2: Create buffers and initialize test values
    // ------------------------------------------------------------------------------------
    // Create the buffers and allocate memory
    cl::Buffer data_packet_buf(context, CL_MEM_READ_ONLY, sizeof(struct SW_Data_Packet), NULL, &err);
    cl::Buffer all_peds_buf(context, CL_MEM_READ_ONLY, sizeof(uint16_t) * 2 * NUM_SAMPLES * NUM_CHANNELS, NULL, &err);
    cl::Buffer bounds_buf(context, CL_MEM_READ_ONLY, sizeof(int) * 8, NULL, &err);
    cl::Buffer output_integrals_buf(context, CL_MEM_WRITE_ONLY, sizeof(int32_t) * 4 * NUM_CHANNELS, NULL, &err);

    // Map buffers to kernel arguments, thereby assigning them to specific device memory banks
    krnl_preprocess.setArg(0, data_packet_buf);
    krnl_preprocess.setArg(1, all_peds_buf);
    krnl_preprocess.setArg(2, bounds_buf);
    krnl_preprocess.setArg(3, output_integrals_buf);

    // Map host-side buffer memory to user-space pointers
    struct SW_Data_Packet * input_data_packet = (struct SW_Data_Packet *)q.enqueueMapBuffer(data_packet_buf, CL_TRUE, CL_MAP_WRITE, 0, sizeof(struct SW_Data_Packet));
    uint16_t *input_all_peds = (uint16_t *)q.enqueueMapBuffer(all_peds_buf, CL_TRUE, CL_MAP_WRITE, 0, sizeof(uint16_t) * 2 * NUM_SAMPLES * NUM_CHANNELS);
    int *bounds = (int *)q.enqueueMapBuffer(bounds_buf, CL_TRUE, CL_MAP_WRITE, 0, sizeof(int) * 8);
    int32_t *output_integrals = (int32_t *)q.enqueueMapBuffer(output_integrals_buf, CL_TRUE, CL_MAP_WRITE | CL_MAP_READ, 0, sizeof(int32_t) * 4 * NUM_CHANNELS);

    // Initialize the data used in the test
    initialize_inputs(input_data_packet, input_all_peds);

    //char b_str[] = {'-5', '5', '-10', '10', '-15', '15', '-20', '20','\0'};
    //char *bounds_strings = b_str;
    const char * bounds_strings[8] = {"-5", "5", "-10", "10", "-15", "15", "-20", "20"}; // {} -> 
    bounds[0] = atoi(bounds_strings[0]);
    bounds[1] = atoi(bounds_strings[1]);
    bounds[2] = atoi(bounds_strings[2]);
    bounds[3] = atoi(bounds_strings[3]);
    bounds[4] = atoi(bounds_strings[4]);
    bounds[5] = atoi(bounds_strings[5]);
    bounds[6] = atoi(bounds_strings[6]);
    bounds[7] = atoi(bounds_strings[7]);

    // ------------------------------------------------------------------------------------
    // Step 3: Run the kernel
    // ------------------------------------------------------------------------------------
    // Set kernel arguments
    krnl_preprocess.setArg(0, data_packet_buf);
    krnl_preprocess.setArg(1, all_peds_buf);
    krnl_preprocess.setArg(2, bounds_buf);
    krnl_preprocess.setArg(3, output_integrals_buf);

    // Schedule transfer of inputs to device memory, execution of kernel, and transfer of outputs back to host memory
    q.enqueueMigrateMemObjects({data_packet_buf, all_peds_buf, bounds_buf}, 0 /* 0 means from host*/); // Send data from host to FPGA
    q.enqueueTask(krnl_preprocess); // Run this kernel (add to task queue)
    q.enqueueMigrateMemObjects({output_integrals_buf}, CL_MIGRATE_MEM_OBJECT_HOST); // Fetching data from FGPA to host

    // Wait for all scheduled operations to finish
    q.finish();

    // ------------------------------------------------------------------------------------
    // Step 4: Check Results and Release Allocated Resources
    // ------------------------------------------------------------------------------------

    produce_output(bounds_strings, output_integrals, input_data_packet);

    /*bool match = true;
    for (int i = 0; i < DATA_SIZE; i++)
    {
        int expected = in1[i] + in2[i];
        if (out[i] != expected)
        {
            std::cout << "Error: Result mismatch" << std::endl;
            std::cout << "i = " << i << " CPU result = " << expected << " Device result = " << out[i] << std::endl;
            match = false;
            break;
        }
    } 

    delete[] fileBuf;

    std::cout << "TEST " << (match ? "PASSED" : "FAILED") << std::endl;
    return (match ? EXIT_SUCCESS : EXIT_FAILURE); */
}

// ------------------------------------------------------------------------------------
// Utility functions
// ------------------------------------------------------------------------------------
std::vector<cl::Device> get_xilinx_devices()
{
    size_t i;
    cl_int err;
    std::vector<cl::Platform> platforms;
    err = cl::Platform::get(&platforms);
    cl::Platform platform;
    for (i = 0; i < platforms.size(); i++)
    {
        platform = platforms[i];
        std::string platformName = platform.getInfo<CL_PLATFORM_NAME>(&err);
        if (platformName == "Xilinx")
        {
            std::cout << "INFO: Found Xilinx Platform" << std::endl;
            break;
        }
    }
    if (i == platforms.size())
    {
        std::cout << "ERROR: Failed to find Xilinx platform" << std::endl;
        exit(EXIT_FAILURE);
    }

    //Getting ACCELERATOR Devices and selecting 1st such device
    std::vector<cl::Device> devices;
    err = platform.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devices);
    return devices;
}

char *read_binary_file(const std::string &xclbin_file_name, unsigned &nb)
{
    if (access(xclbin_file_name.c_str(), R_OK) != 0)
    {
        printf("ERROR: %s xclbin not available please build\n", xclbin_file_name.c_str());
        exit(EXIT_FAILURE);
    }
    //Loading XCL Bin into char buffer
    std::cout << "INFO: Loading '" << xclbin_file_name << "'\n";
    std::ifstream bin_file(xclbin_file_name.c_str(), std::ifstream::binary);
    bin_file.seekg(0, bin_file.end);
    nb = bin_file.tellg();
    bin_file.seekg(0, bin_file.beg);
    char *buf = new char[nb];
    bin_file.read(buf, nb);
    return buf;
}
