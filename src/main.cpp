#include <iostream>
#include <string>
#include "compressor.h"

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " <command> [options]\n"
              << "Commands:\n"
              << "  train <trace_file> <output_config>\n"
              << "  compress <trace_file> <output_file> <config_file>\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command == "train") {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        std::string trace_path = argv[2];
        std::string config_path = argv[3];
        try {
            train_compressor(trace_path, config_path);
        } catch (const std::exception& e) {
            std::cerr << "Error during training: " << e.what() << "\n";
            return 1;
        }
    } else if (command == "compress") {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string trace_path = argv[2];
        std::string output_path = argv[3];
        std::string config_path = argv[4];
        try {
            compress_trace(trace_path, output_path, config_path);
        } catch (const std::exception& e) {
             std::cerr << "Error during compression: " << e.what() << "\n";
             return 1;
        }
    } else if (command == "decompress") {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string compressed_path = argv[2];
        std::string output_path = argv[3];
        std::string config_path = argv[4];
        try {
            decompress_trace(compressed_path, output_path, config_path);
        } catch (const std::exception& e) {
             std::cerr << "Error during decompression: " << e.what() << "\n";
             return 1;
        }
    } else if (command == "verify") {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string trace_path = argv[2];
        std::string compressed_path = argv[3];
        std::string config_path = argv[4];
        try {
            verify_trace(trace_path, compressed_path, config_path);
        } catch (const std::exception& e) {
             std::cerr << "Error during verification: " << e.what() << "\n";
             return 1;
        }
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
