#ifndef FILEPATH_H
#define FILEPATH_H

#include <string>

//TODO: Update the following path!
// const std::string path_to_repo = "/home/yourusername/adapt_fpga/";
const std::string path_to_repo = "/home/warehouse/msudvarg/capstone_sp23/src/";
const std::string str_packet_file = path_to_repo + "EventStream.dat";
const std::string str_ped_file = path_to_repo + "peds.dat";
const std::string str_output_file = path_to_repo + "output.txt";
const char * packet_file = str_packet_file.c_str();
const char * ped_file = str_ped_file.c_str();
const char * output_file = str_output_file.c_str();
#endif