#include "compressor.h"
#include "common.h"

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <deque>
#include <future>

#include "openzl/cpp/CCtx.hpp"
#include "openzl/zl_compress.h"
#include "tools/training/utils/thread_pool.h"

using namespace openzl;
using namespace tracezl;

void compress_trace(const std::string& trace_path, const std::string& output_path,
                    const std::string& config_path, size_t chunk_size, size_t num_threads) {
    std::cout << "Compressing " << trace_path << " with " << num_threads << " threads..."
              << std::endl;

    // Load config
    std::ifstream configFile(config_path, std::ios::binary | std::ios::ate);
    if (!configFile) throw std::runtime_error("Cannot open config file");
    size_t configSize = configFile.tellg();
    configFile.seekg(0);
    std::string configData(configSize, '\0');
    configFile.read(&configData[0], configSize);

    // Setup compressor (shared across threads)
    auto compressor = createCompressorFromSerialized(configData);
    compressor->setParameter(CParam::FormatVersion, ZL_MAX_FORMAT_VERSION);

    // Open Input File
    std::ifstream inFile(trace_path, std::ios::binary | std::ios::ate);
    if (!inFile) throw std::runtime_error("Cannot open trace file");
    size_t totalSize = inFile.tellg();
    inFile.seekg(0);

    // Open Output File
    std::ofstream outFile(output_path, std::ios::binary);
    if (!outFile) throw std::runtime_error("Cannot open output file");

    // Thread Pool
    openzl::training::ThreadPool pool(num_threads);
    std::deque<std::future<std::string>> futures;
    const size_t max_queue_size = num_threads * 2;

    size_t processed = 0;
    size_t totalCompressed = 0;

    while (processed < totalSize) {
        // Flow control: if queue is full, write one result
        if (futures.size() >= max_queue_size) {
            std::string result = futures.front().get();
            futures.pop_front();

            outFile.write(result.data(), result.size());
            totalCompressed += result.size();
        }

        size_t remaining = totalSize - processed;
        size_t toRead = std::min(remaining, chunk_size);

        // Align to 64 bytes
        if (toRead % 64 != 0 && toRead < remaining) {
            toRead = (toRead / 64) * 64;
        }

        // Read chunk
        std::vector<char> buffer(toRead);
        inFile.read(buffer.data(), toRead);
        processed += toRead;

        // Submit task
        // We capture compressor by raw pointer. The main thread outlives the tasks.
        Compressor* rawCompressor = compressor.get();

        futures.push_back(pool.run([rawCompressor, data = std::move(buffer)]() -> std::string {
            CCtx cctx;
            cctx.refCompressor(*rawCompressor);
            cctx.setParameter(CParam::FormatVersion, ZL_MAX_FORMAT_VERSION);
            cctx.setParameter(CParam::StickyParameters, 1);  // Sticky local to this CCtx, fine.

            size_t bound = ZL_compressBound(data.size());
            std::string compressed(bound, '\0');

            ZL_Report res =
                ZL_CCtx_compress(cctx.get(), compressed.data(), bound, data.data(), data.size());
            size_t cSize = cctx.unwrap(res, "Compression failed");
            compressed.resize(cSize);
            return compressed;
        }));

        std::cout << "\rSubmitted: " << (processed * 100 / totalSize) << "%" << std::flush;
    }

    // Drain remaining futures
    while (!futures.empty()) {
        std::string result = futures.front().get();
        futures.pop_front();
        outFile.write(result.data(), result.size());
        totalCompressed += result.size();
    }

    std::cout << std::endl;
    std::cout << "Compressed size: " << totalCompressed
              << " bytes (Ratio: " << (double)totalSize / totalCompressed << ")" << std::endl;
}
