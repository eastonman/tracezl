#include "common.h"

#include <cassert>
#include <vector>

#include "champsim_trace.h"
#include "openzl/codecs/zl_ace.h"
#include "openzl/zl_errors.h"

namespace tracezl {

using namespace openzl;

// Dispatch function to split ChampSim trace struct into fields
ZL_Report traceDispatchFn(ZL_Graph* graph, ZL_Edge* inputEdges[], size_t numInputs) noexcept {
    ZL_RESULT_DECLARE_SCOPE_REPORT(graph);

    assert(numInputs == 1);
    const ZL_Input* const input = ZL_Edge_getData(inputEdges[0]);
    // const uint8_t* const inputData = (const uint8_t*)ZL_Input_ptr(input);
    const size_t inputSize = ZL_Input_numElts(input);

    // ChampSim trace instruction size is 64 bytes
    const size_t instrSize = sizeof(trace_instr_format_t);

    // if (inputSize % instrSize != 0) { ... }

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

}  // namespace tracezl
