#include "compressor.h"
#include "common.h"

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <deque>
#include <future>

#include "openzl/cpp/DCtx.hpp"
#include "openzl/zl_decompress.h"
#include "tools/training/utils/thread_pool.h"

using namespace openzl;
using namespace tracezl;

void decompress_trace(const std::string& compressed_path, const std::string& output_path,
                      const std::string& config_path, size_t chunk_size, size_t num_threads) {
    std::cout << "Decompressing " << compressed_path << " to " << output_path << " with "
              << num_threads << " threads..." << std::endl;

    // Load config
    std::ifstream configFile(config_path, std::ios::binary | std::ios::ate);
    if (!configFile) throw std::runtime_error("Cannot open config file");
    size_t configSize = configFile.tellg();
    configFile.seekg(0);
    std::string configData(configSize, '\0');
    configFile.read(&configData[0], configSize);

    auto compressor = createCompressorFromSerialized(configData);

    // Open Files
    std::ifstream compFile(compressed_path, std::ios::binary | std::ios::ate);
    if (!compFile) throw std::runtime_error("Cannot open compressed file");
    size_t compFileSize = compFile.tellg();
    compFile.seekg(0);

    std::ofstream outFile(output_path, std::ios::binary);
    if (!outFile) throw std::runtime_error("Cannot open output file");

    // Thread Pool
    openzl::training::ThreadPool pool(num_threads);
    std::deque<std::future<std::string>> futures;
    const size_t max_queue_size = num_threads * 2;

    // Buffer for reading compressed data
    std::vector<char> buffer;
    buffer.reserve(chunk_size + 4096);

    size_t bufferPos = 0;
    size_t compProcessed = 0;
    size_t totalDecompressed = 0;

    while (true) {
        // Flow control
        if (futures.size() >= max_queue_size) {
            std::string result = futures.front().get();
            futures.pop_front();
            outFile.write(result.data(), result.size());
            totalDecompressed += result.size();
        }

        // Ensure we have data
        if (buffer.size() - bufferPos < 64) {
            if (bufferPos > 0) {
                buffer.erase(buffer.begin(), buffer.begin() + bufferPos);
                bufferPos = 0;
            }
            size_t currentSize = buffer.size();
            buffer.resize(currentSize + 64 * 1024);
            compFile.read(buffer.data() + currentSize, 64 * 1024);
            size_t read = compFile.gcount();
            buffer.resize(currentSize + read);

            if (read == 0 && buffer.empty()) break;
        }

        if (buffer.empty()) break;

        // Check frame size
        ZL_Report sizeReport =
            ZL_getCompressedSize(buffer.data() + bufferPos, buffer.size() - bufferPos);

        if (ZL_isError(sizeReport)) {
            if (compFile.eof()) {
                throw std::runtime_error(
                    "Corrupt compressed file or truncated frame header at offset " +
                    std::to_string(compProcessed));
            }
            if (bufferPos > 0) {
                buffer.erase(buffer.begin(), buffer.begin() + bufferPos);
                bufferPos = 0;
            }
            size_t currentSize = buffer.size();
            size_t toRead = 1024 * 1024;
            buffer.resize(currentSize + toRead);
            compFile.read(buffer.data() + currentSize, toRead);
            size_t read = compFile.gcount();
            buffer.resize(currentSize + read);
            continue;
        }

        size_t cSize = ZL_RES_value(sizeReport);

        // Ensure full frame
        while (buffer.size() - bufferPos < cSize) {
            if (compFile.eof()) {
                throw std::runtime_error("Unexpected EOF: Compressed frame requires " +
                                         std::to_string(cSize) + " bytes.");
            }
            if (bufferPos > 0) {
                buffer.erase(buffer.begin(), buffer.begin() + bufferPos);
                bufferPos = 0;
            }
            size_t needed = cSize - buffer.size();
            size_t toRead = std::max(needed, size_t(1024 * 1024));
            size_t currentSize = buffer.size();
            buffer.resize(currentSize + toRead);
            compFile.read(buffer.data() + currentSize, toRead);
            size_t read = compFile.gcount();
            buffer.resize(currentSize + read);
        }

        // Copy frame data for task
        std::vector<char> frameData(buffer.begin() + bufferPos, buffer.begin() + bufferPos + cSize);

        // Submit task
        futures.push_back(pool.run([frame = std::move(frameData)]() -> std::string {
            DCtx dctx;
            return dctx.decompressSerial(std::string_view(frame.data(), frame.size()));
        }));

        bufferPos += cSize;
        compProcessed += cSize;

        std::cout << "\rSubmitted: " << (compProcessed * 100 / compFileSize) << "%" << std::flush;
    }

    // Drain
    while (!futures.empty()) {
        std::string result = futures.front().get();
        futures.pop_front();
        outFile.write(result.data(), result.size());
        totalDecompressed += result.size();
    }

    std::cout << std::endl;
    std::cout << "Decompression complete. Recovered " << totalDecompressed << " bytes."
              << std::endl;
}
