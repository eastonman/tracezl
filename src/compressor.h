#pragma once
#include <string>

void train_compressor(const std::string& trace_path, const std::string& config_path);
void compress_trace(const std::string& trace_path, const std::string& output_path,
                    const std::string& config_path);
void verify_trace(const std::string& trace_path, const std::string& compressed_path,
                  const std::string& config_path);
void decompress_trace(const std::string& compressed_path, const std::string& output_path,
                      const std::string& config_path);
