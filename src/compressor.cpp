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
#include "openzl/zl_errors.h"
#include "openzl/zl_graph_api.h"
#include "tools/io/InputSetBuilder.h"
#include "tools/training/train.h"

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

        // Set metadata for clustering to identify the field
        ZL_ERR_IF_ERR(ZL_Edge_setIntMetadata(convertedEdge, ZL_CLUSTERING_TAG_METADATA_ID, i));

        outputEdges.push_back(convertedEdge);
    }

    // Get custom graph (Clustering)
    ZL_GraphIDList customGraphs = ZL_Graph_getCustomGraphs(graph);
    ZL_ERR_IF_NE(customGraphs.nbGraphIDs, 1, graphParameter_invalid);

    // Send all fields to the clustering graph
    ZL_ERR_IF_ERR(ZL_Edge_setParameterizedDestination(outputEdges.data(), outputEdges.size(),
                                                      customGraphs.graphids[0], NULL));

    return ZL_returnSuccess();
}

ZL_GraphID registerGraph(Compressor& compressor) {
    // 1. Register the Clustering Graph
    ZL_ClusteringConfig defaultConfig = {.nbClusters = 0, .nbTypeDefaults = 0};

    // Successors to try for each cluster
    std::vector<ZL_GraphID> successors = {
        ZL_GRAPH_STORE,             // Uncompressed
        ZL_GRAPH_ZSTD,              // Zstd
        ZL_GRAPH_COMPRESS_GENERIC,  // Generic OpenZL
        // Delta encoding then LZ is good for addresses
        ZL_Compressor_registerStaticGraph_fromNode1o(compressor.get(), ZL_NODE_DELTA_INT,
                                                     ZL_GRAPH_FIELD_LZ)};

    ZL_GraphID clusteringGraph = ZL_Clustering_registerGraph(compressor.get(), &defaultConfig,
                                                             successors.data(), successors.size());

    // 2. Register our Parsing/Dispatch Graph
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

    // 3. Parameterize the parsing graph with the clustering graph as its custom target
    std::vector<ZL_GraphID> customGraphs = {clusteringGraph};
    GraphParameters params = {.customGraphs = std::move(customGraphs)};

    return compressor.parameterizeGraph(parsingGraph.value(), params);
}

std::unique_ptr<Compressor> createCompressorFromSerialized(poly::string_view serialized) {
    auto compressor = std::make_unique<Compressor>();
    registerGraph(*compressor);
    compressor->deserialize(serialized);
    return compressor;
}

}  // namespace

void train_compressor(const std::string& trace_path, const std::string& config_path) {
    std::cout << "Training compressor on " << trace_path << "..." << std::endl;

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
        .threads = 4,
        .clusteringTrainer = openzl::training::ClusteringTrainer::Greedy};

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
                    const std::string& config_path, size_t chunk_size) {
    std::cout << "Compressing " << trace_path << "..." << std::endl;

    // Load config
    std::ifstream configFile(config_path, std::ios::binary | std::ios::ate);
    if (!configFile) throw std::runtime_error("Cannot open config file");
    size_t configSize = configFile.tellg();
    configFile.seekg(0);
    std::string configData(configSize, '\0');
    configFile.read(&configData[0], configSize);

    // Setup compressor
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

    // Context
    CCtx cctx;
    cctx.refCompressor(*compressor);
    cctx.setParameter(CParam::FormatVersion, ZL_MAX_FORMAT_VERSION);
    cctx.setParameter(CParam::StickyParameters, 1);

    // Chunk Processing
    std::vector<char> buffer(chunk_size);
    size_t processed = 0;
    size_t totalCompressed = 0;

    while (processed < totalSize) {
        size_t remaining = totalSize - processed;
        size_t toRead = std::min(remaining, chunk_size);

        // Ensure we align to instruction boundaries (64 bytes)
        // Though standard CHUNK_SIZE (100MB) is divisible by 64.
        if (toRead % 64 != 0) {
            // Adjust toRead to be multiple of 64 if not at end
            if (toRead < remaining) {
                toRead = (toRead / 64) * 64;
            }
        }

        inFile.read(buffer.data(), toRead);

        // cctx.compressSerial fails if refCompressor was called because it expects no graph to be
        // set. ZL_CCtx_compress works with attached compressor.
        size_t bound = ZL_compressBound(toRead);
        std::string compressed(bound, '\0');

        ZL_Report res =
            ZL_CCtx_compress(cctx.get(), compressed.data(), bound, buffer.data(), toRead);

        size_t cSize = cctx.unwrap(res, "Compression failed");
        compressed.resize(cSize);

        // Frame Format: [Size (8 bytes)][Data]
        uint64_t frameSize = cSize;
        outFile.write(reinterpret_cast<const char*>(&frameSize), sizeof(frameSize));
        outFile.write(compressed.data(), cSize);

        totalCompressed += sizeof(frameSize) + cSize;
        processed += toRead;

        std::cout << "\rProgress: " << (processed * 100 / totalSize) << "%" << std::flush;
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

    std::vector<char> origBuffer(chunk_size);
    std::vector<char> compBuffer;  // Resizable

    size_t compProcessed = 0;
    size_t origProcessed = 0;

    while (compProcessed < compFileSize) {
        // Read Compressed Frame Size
        uint64_t cSize;
        if (compFile.read(reinterpret_cast<char*>(&cSize), sizeof(cSize)).gcount() !=
            sizeof(cSize)) {
            throw std::runtime_error("Unexpected end of compressed file while reading frame size");
        }
        compProcessed += sizeof(cSize);

        // Read Compressed Data
        compBuffer.resize(cSize);
        if (compFile.read(compBuffer.data(), cSize).gcount() != cSize) {
            throw std::runtime_error("Unexpected end of compressed file while reading frame data");
        }
        compProcessed += cSize;

        // Decompress
        std::string decompressed =
            dctx.decompressSerial(std::string_view(compBuffer.data(), cSize));
        size_t dSize = decompressed.size();

        // Verify against original
        // Ensure we have enough original data to compare
        if (origProcessed + dSize > origFileSize) {
            throw std::runtime_error("Decompressed data exceeds original file size");
        }

        // Read corresponding original chunk
        // We might need to resize origBuffer if dSize > chunk_size (unlikely if we compressed
        // chunks of chunk_size)
        if (dSize > origBuffer.size()) origBuffer.resize(dSize);

        origFile.read(origBuffer.data(), dSize);
        if (memcmp(decompressed.data(), origBuffer.data(), dSize) != 0) {
            throw std::runtime_error("Content mismatch at offset " + std::to_string(origProcessed));
        }

        origProcessed += dSize;

        std::cout << "\rVerified: " << (compProcessed * 100 / compFileSize) << "%" << std::flush;
    }

    if (origProcessed != origFileSize) {
        throw std::runtime_error("Verification incomplete: Original file has more data");
    }

    std::cout << std::endl << "Verification successful!" << std::endl;
}

void decompress_trace(const std::string& compressed_path, const std::string& output_path,
                      const std::string& config_path, size_t chunk_size) {
    std::cout << "Decompressing " << compressed_path << " to " << output_path << "..." << std::endl;

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

    // Decompress Context
    DCtx dctx;

    std::vector<char> compBuffer;
    size_t compProcessed = 0;
    size_t totalDecompressed = 0;

    // Try to detect framing format
    // Read first 8 bytes
    if (compFileSize < 8) throw std::runtime_error("File too small");

    uint64_t firstQWord;
    compFile.read(reinterpret_cast<char*>(&firstQWord), 8);
    compFile.seekg(0);

    // Check if first 4 bytes match ZStrong Magic Number (0xD7B1A5D6)
    // Little Endian: D6 A5 B1 D7
    uint32_t magic = 0xD7B1A5D6;
    uint32_t fileMagic;
    compFile.read(reinterpret_cast<char*>(&fileMagic), 4);
    compFile.seekg(0);

    bool hasSizePrefix = true;
    if (fileMagic == magic) {
        std::cout
            << "File starts with ZStrong Magic Number. Assuming no size prefix (single chunk)."
            << std::endl;
        hasSizePrefix = false;
    }

    if (!hasSizePrefix) {
        // Assume single chunk
        compBuffer.resize(compFileSize);
        compFile.read(compBuffer.data(), compFileSize);

        std::string decompressed =
            dctx.decompressSerial(std::string_view(compBuffer.data(), compFileSize));
        outFile.write(decompressed.data(), decompressed.size());
        totalDecompressed += decompressed.size();
    } else {
        while (compProcessed < compFileSize) {
            // Read Compressed Frame Size
            uint64_t cSize;
            if (compFile.read(reinterpret_cast<char*>(&cSize), sizeof(cSize)).gcount() !=
                sizeof(cSize)) {
                break;  // EOF
            }
            compProcessed += sizeof(cSize);

            // Sanity check cSize
            if (cSize > ZL_compressBound(chunk_size) && cSize > 200 * 1024 * 1024) {
                std::cerr << "Error: Compressed chunk size " << cSize << " is invalid (too large)."
                          << std::endl;
                throw std::runtime_error(
                    "Corrupt compressed file: invalid chunk size or unknown format");
            }

            // Read Compressed Data
            try {
                compBuffer.resize(cSize);
            } catch (const std::bad_alloc& e) {
                throw std::runtime_error("Failed to allocate buffer for compressed chunk of size " +
                                         std::to_string(cSize));
            }

            if (compFile.read(compBuffer.data(), cSize).gcount() != cSize) {
                throw std::runtime_error(
                    "Unexpected end of compressed file while reading frame data");
            }
            compProcessed += cSize;

            // Decompress
            std::string decompressed =
                dctx.decompressSerial(std::string_view(compBuffer.data(), cSize));

            outFile.write(decompressed.data(), decompressed.size());
            totalDecompressed += decompressed.size();

            std::cout << "\rProgress: " << (compProcessed * 100 / compFileSize) << "%"
                      << std::flush;
        }
    }

    std::cout << std::endl;
    std::cout << "Decompression complete. Recovered " << totalDecompressed << " bytes."
              << std::endl;
}
