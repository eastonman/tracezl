#pragma once
#include <string>

void train_compressor(const std::string& trace_path, const std::string& config_path);
void compress_trace(const std::string& trace_path, const std::string& output_path,
                    const std::string& config_path, size_t chunk_size = 100 * 1024 * 1024);
void verify_trace(const std::string& trace_path, const std::string& compressed_path,
                  const std::string& config_path, size_t chunk_size = 100 * 1024 * 1024);
void decompress_trace(const std::string& compressed_path, const std::string& output_path,
                      const std::string& config_path, size_t chunk_size = 100 * 1024 * 1024);
