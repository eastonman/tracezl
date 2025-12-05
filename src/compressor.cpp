#include "compressor.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>  // For std::bad_alloc
#include <unordered_map>
#include <vector>

#include "champsim_trace.h"
#include "openzl/codecs/zl_clustering.h"
#include "openzl/cpp/CCtx.hpp"
#include "openzl/cpp/Compressor.hpp"
#include "openzl/cpp/DCtx.hpp"
#include "openzl/cpp/Input.hpp"
#include "openzl/zl_compress.h"
#include "openzl/zl_compressor.h"
#include "openzl/zl_decompress.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_graph_api.h"
#include "openzl/codecs/zl_ace.h"
#include "tools/io/InputSetBuilder.h"
#include "tools/training/train.h"
#include "tools/training/utils/thread_pool.h"

namespace {

using namespace openzl;

// Define tags for our fields
enum FieldTag {
    TAG_IP = 0,
    TAG_IS_BRANCH,
    TAG_BRANCH_TAKEN,
    TAG_DEST_REGS,
    TAG_SOURCE_REGS,
    TAG_DEST_MEM,
    TAG_SOURCE_MEM,
    NUM_TAGS
};

// Dispatch function to split ChampSim trace struct into fields
ZL_Report traceDispatchFn(ZL_Graph* graph, ZL_Edge* inputEdges[], size_t numInputs) noexcept {
    ZL_RESULT_DECLARE_SCOPE_REPORT(graph);

    assert(numInputs == 1);
    const ZL_Input* const input = ZL_Edge_getData(inputEdges[0]);
    const uint8_t* const inputData = (const uint8_t*)ZL_Input_ptr(input);
    const size_t inputSize = ZL_Input_numElts(input);

    // ChampSim trace instruction size is 64 bytes
    const size_t instrSize = sizeof(trace_instr_format_t);

    if (inputSize % instrSize != 0) {
        // Use a generic error if input is not aligned (though OpenZL handles partials gracefully
        // usually, strict alignment is better for this fixed struct)
        // ZL_ERR_RETURN(srcSize_tooSmall); // Or similar
    }

    size_t numInstrs = inputSize / instrSize;

    // We need to construct the dispatch arrays
    // For each instruction, we emit segments for each field
    std::vector<unsigned> tags;
    std::vector<size_t> sizes;
    tags.reserve(numInstrs * NUM_TAGS);
    sizes.reserve(numInstrs * NUM_TAGS);

    for (size_t i = 0; i < numInstrs; ++i) {
        // IP: 8 bytes
        tags.push_back(TAG_IP);
        sizes.push_back(8);
        // is_branch: 1 byte
        tags.push_back(TAG_IS_BRANCH);
        sizes.push_back(1);
        // branch_taken: 1 byte
        tags.push_back(TAG_BRANCH_TAKEN);
        sizes.push_back(1);
        // dest_regs: 2 bytes
        tags.push_back(TAG_DEST_REGS);
        sizes.push_back(2);
        // source_regs: 4 bytes
        tags.push_back(TAG_SOURCE_REGS);
        sizes.push_back(4);
        // dest_mem: 16 bytes
        tags.push_back(TAG_DEST_MEM);
        sizes.push_back(16);
        // source_mem: 32 bytes
        tags.push_back(TAG_SOURCE_MEM);
        sizes.push_back(32);
    }

    ZL_DispatchInstructions instructions = {.segmentSizes = sizes.data(),
                                            .tags = tags.data(),
                                            .nbSegments = sizes.size(),
                                            .nbTags = NUM_TAGS};

    ZL_TRY_LET(ZL_EdgeList, dispatchEdges, ZL_Edge_runDispatchNode(inputEdges[0], &instructions));

    // Expect NUM_TAGS + 2 output edges
    // Edge 0: Tags stream
    // Edge 1: Sizes stream
    // Edge 2..8: Data streams for tags 0..6

    // Route tags and sizes to generic compressor
    ZL_ERR_IF_ERR(ZL_Edge_setDestination(dispatchEdges.edges[0], ZL_GRAPH_COMPRESS_GENERIC));
    ZL_ERR_IF_ERR(ZL_Edge_setDestination(dispatchEdges.edges[1], ZL_GRAPH_COMPRESS_GENERIC));

    // Prepare data edges for clustering
    std::vector<ZL_Edge*> outputEdges;
    outputEdges.reserve(NUM_TAGS);

    // Define element widths for interpretation (in bytes)
    // IP(8), IsBr(1), BrTk(1), DRegs(1), SRegs(1), DMem(8), SMem(8)
    // Note: DMem/SMem are arrays of uint64, so element width is 8.
    // Regs are arrays of uint8, so element width is 1.
    const size_t eltWidths[NUM_TAGS] = {8, 1, 1, 1, 1, 8, 8};

    for (int i = 0; i < NUM_TAGS; ++i) {
        ZL_Edge* dataEdge = dispatchEdges.edges[2 + i];

        // Interpret as LE numeric stream
        ZL_NodeID node = ZL_Node_interpretAsLE(eltWidths[i] * 8);
        ZL_TRY_LET(ZL_EdgeList, convertEdges, ZL_Edge_runNode(dataEdge, node));
        assert(convertEdges.nbEdges == 1);

        ZL_Edge* convertedEdge = convertEdges.edges[0];

        outputEdges.push_back(convertedEdge);
    }

    // Get custom graphs (ACE graphs for each field)
    ZL_GraphIDList customGraphs = ZL_Graph_getCustomGraphs(graph);
    ZL_ERR_IF_NE(customGraphs.nbGraphIDs, NUM_TAGS, graphParameter_invalid);

    // Send each field to its dedicated ACE graph
    for (int i = 0; i < NUM_TAGS; ++i) {
        ZL_ERR_IF_ERR(ZL_Edge_setDestination(outputEdges[i], customGraphs.graphids[i]));
    }

    return ZL_returnSuccess();
}

ZL_GraphID registerGraph(Compressor& compressor) {
    // Create an ACE graph for each field type
    std::vector<ZL_GraphID> aceGraphs;
    for (int i = 0; i < NUM_TAGS; ++i) {
        aceGraphs.push_back(ZL_Compressor_buildACEGraph(compressor.get()));
    }

    // Register our Parsing/Dispatch Graph
    auto parsingGraphName = "ChampSimTraceParser";
    auto parsingGraph = compressor.getGraph(parsingGraphName);

    if (!parsingGraph) {
        ZL_Type inputTypeMask = ZL_Type_serial;
        ZL_FunctionGraphDesc desc = {.name = parsingGraphName,
                                     .graph_f = traceDispatchFn,
                                     .inputTypeMasks = &inputTypeMask,
                                     .nbInputs = 1,
                                     .customGraphs = NULL,
                                     .nbCustomGraphs = 0,
                                     .localParams = {}};
        parsingGraph = compressor.registerFunctionGraph(desc);
    }

    // Parameterize the parsing graph with the ACE graphs as custom targets
    GraphParameters params = {.customGraphs = std::move(aceGraphs)};

    return compressor.parameterizeGraph(parsingGraph.value(), params);
}

std::unique_ptr<Compressor> createCompressorFromSerialized(poly::string_view serialized) {
    auto compressor = std::make_unique<Compressor>();
    registerGraph(*compressor);
    compressor->deserialize(serialized);
    return compressor;
}

}  // namespace

void train_compressor(const std::string& trace_path, const std::string& config_path,
                      size_t num_threads) {
    std::cout << "Training compressor on " << trace_path << " with " << num_threads << " threads..." << std::endl;

    // Prepare input
    // We use InputSetBuilder to load the file
    openzl::tools::io::InputSetBuilder builder(true);
    builder.add_path(trace_path);
    auto inputs = std::move(builder).build();

    // Prepare base compressor
    Compressor compressor;
    ZL_GraphID startGraph = registerGraph(compressor);

    // Select starting graph
    openzl::unwrap(ZL_Compressor_selectStartingGraphID(compressor.get(), startGraph),
                   "Failed to select starting graph");

    // Train Params
    openzl::training::TrainParams params = {
        .compressorGenFunc = createCompressorFromSerialized,
        .threads = (uint32_t)num_threads,
        .noClustering = true,
        .paretoFrontier = true};

    // Convert inputs
    auto multiInputs = openzl::training::inputSetToMultiInputs(*inputs);

    // Run training
    auto result = openzl::training::train(multiInputs, compressor, params);

    if (result.empty()) {
        throw std::runtime_error("Training failed to produce any compressor");
    }

    // Save result
    std::ofstream out(config_path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output config file");
    out << *result[0];
    out.close();

    std::cout << "Training complete. Config saved to " << config_path << std::endl;
}

void compress_trace(const std::string& trace_path, const std::string& output_path,
                    const std::string& config_path, size_t chunk_size, size_t num_threads) {
    std::cout << "Compressing " << trace_path << " with " << num_threads << " threads..." << std::endl;

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
            
            uint64_t frameSize = result.size(); // Though we don't write frame size prefix anymore? 
            // Wait, in previous step I REMOVED the 8-byte size prefix.
            // "Write compressed frame directly (OpenZL frames are self-describing)"
            // So just write data.
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
            cctx.setParameter(CParam::StickyParameters, 1); // Sticky local to this CCtx, fine.

            size_t bound = ZL_compressBound(data.size());
            std::string compressed(bound, '\0');

            ZL_Report res = ZL_CCtx_compress(cctx.get(), compressed.data(), bound, data.data(), data.size());
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

void verify_trace(const std::string& trace_path, const std::string& compressed_path,
                  const std::string& config_path, size_t chunk_size) {
    std::cout << "Verifying " << compressed_path << " against " << trace_path << "..." << std::endl;

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

    std::ifstream origFile(trace_path, std::ios::binary | std::ios::ate);
    if (!origFile) throw std::runtime_error("Cannot open original trace file");
    size_t origFileSize = origFile.tellg();
    origFile.seekg(0);

    // Decompress Context
    DCtx dctx;

    std::vector<char> origBuffer;
    std::vector<char> buffer;
    buffer.reserve(chunk_size + 4096); 
    
    size_t bufferPos = 0; 
    size_t compProcessed = 0;
    size_t origProcessed = 0;

    while (true) {
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
        ZL_Report sizeReport = ZL_getCompressedSize(buffer.data() + bufferPos, buffer.size() - bufferPos);
        
        if (ZL_isError(sizeReport)) {
            if (compFile.eof()) {
                 throw std::runtime_error("Corrupt compressed file or truncated frame header at offset " + std::to_string(compProcessed));
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
                throw std::runtime_error("Unexpected EOF: Compressed frame requires " + std::to_string(cSize) + " bytes.");
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

        // Decompress
        std::string decompressed = dctx.decompressSerial(std::string_view(buffer.data() + bufferPos, cSize));
        size_t dSize = decompressed.size();

        // Verify
        if (origProcessed + dSize > origFileSize) {
            throw std::runtime_error("Decompressed data exceeds original file size");
        }

        if (dSize > origBuffer.size()) origBuffer.resize(dSize);
        origFile.read(origBuffer.data(), dSize);
        
        if (memcmp(decompressed.data(), origBuffer.data(), dSize) != 0) {
            throw std::runtime_error("Content mismatch at offset " + std::to_string(origProcessed));
        }

        origProcessed += dSize;
        bufferPos += cSize;
        compProcessed += cSize;

        std::cout << "\rVerified: " << (compProcessed * 100 / compFileSize) << "%" << std::flush;
    }

    if (origProcessed != origFileSize) {
        throw std::runtime_error("Verification incomplete: Original file has more data");
    }

    std::cout << std::endl << "Verification successful!" << std::endl;
}

void decompress_trace(const std::string& compressed_path, const std::string& output_path,
                      const std::string& config_path, size_t chunk_size, size_t num_threads) {
    std::cout << "Decompressing " << compressed_path << " to " << output_path << " with " << num_threads << " threads..." << std::endl;

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
        ZL_Report sizeReport = ZL_getCompressedSize(buffer.data() + bufferPos, buffer.size() - bufferPos);
        
        if (ZL_isError(sizeReport)) {
            if (compFile.eof()) {
                 throw std::runtime_error("Corrupt compressed file or truncated frame header at offset " + std::to_string(compProcessed));
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
                throw std::runtime_error("Unexpected EOF: Compressed frame requires " + std::to_string(cSize) + " bytes.");
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
