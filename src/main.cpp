#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <thread>

#include "compressor.h"

int main(int argc, char** argv) {
    CLI::App app{"tracezl - Trace Compression Tool using OpenZL"};
    app.require_subcommand(1);

    // Common variables used across subcommands
    std::string trace_path;
    std::string config_path;
    std::string output_path;
    std::string compressed_path;
    size_t chunk_size = 100 * 1024 * 1024;  // Default 100MB
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    // Train command
    auto train = app.add_subcommand("train", "Train the compressor model");
    train->add_option("trace_file", trace_path, "Path to the input trace file")->required();
    train->add_option("output_config", config_path, "Path to save the output configuration")
        ->required();
    train->add_option("-t,--threads", num_threads,
                      "Number of threads to use (default: hardware concurrency)");
    train->callback([&]() {
        try {
            train_compressor(trace_path, config_path, num_threads);
        } catch (const std::exception& e) {
            std::cerr << "Error during training: " << e.what() << "\n";
            exit(1);
        }
    });

    // Compress command
    auto compress = app.add_subcommand("compress", "Compress a trace file");
    compress->add_option("trace_file", trace_path, "Path to the input trace file")->required();
    compress->add_option("output_file", output_path, "Path to save the compressed output")
        ->required();
    compress->add_option("config_file", config_path, "Path to the configuration file")->required();
    compress->add_option("-s,--chunk-size", chunk_size, "Chunk size in bytes (default: 100MB)");
    compress->add_option("-t,--threads", num_threads,
                         "Number of threads to use (default: hardware concurrency)");
    compress->callback([&]() {
        try {
            compress_trace(trace_path, output_path, config_path, chunk_size, num_threads);
        } catch (const std::exception& e) {
            std::cerr << "Error during compression: " << e.what() << "\n";
            exit(1);
        }
    });

    // Decompress command
    auto decompress = app.add_subcommand("decompress", "Decompress a trace file");
    decompress->add_option("compressed_file", compressed_path, "Path to the compressed input file")
        ->required();
    decompress->add_option("output_file", output_path, "Path to save the decompressed trace")
        ->required();
    decompress->add_option("config_file", config_path, "Path to the configuration file")
        ->required();
    decompress->add_option("-s,--chunk-size", chunk_size, "Chunk size in bytes (default: 100MB)");
    decompress->add_option("-t,--threads", num_threads,
                           "Number of threads to use (default: hardware concurrency)");
    decompress->callback([&]() {
        try {
            decompress_trace(compressed_path, output_path, config_path, chunk_size, num_threads);
        } catch (const std::exception& e) {
            std::cerr << "Error during decompression: " << e.what() << "\n";
            exit(1);
        }
    });

    // Verify command
    auto verify = app.add_subcommand("verify", "Verify compression integrity");
    verify->add_option("trace_file", trace_path, "Path to the original trace file")->required();
    verify->add_option("compressed_file", compressed_path, "Path to the compressed file")
        ->required();
    verify->add_option("config_file", config_path, "Path to the configuration file")->required();
    verify->add_option("-s,--chunk-size", chunk_size, "Chunk size in bytes (default: 100MB)");
    verify->add_option("-t,--threads", num_threads,
                       "Number of threads to use (default: hardware concurrency)");
    verify->callback([&]() {
        try {
            verify_trace(trace_path, compressed_path, config_path, chunk_size);
        } catch (const std::exception& e) {
            std::cerr << "Error during verification: " << e.what() << "\n";
            exit(1);
        }
    });

    CLI11_PARSE(app, argc, argv);

    return 0;
}
