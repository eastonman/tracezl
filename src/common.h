#pragma once

#include <memory>

#include "openzl/cpp/Compressor.hpp"
#include "openzl/zl_graph_api.h"

namespace tracezl {

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

// Common helper functions
ZL_Report traceDispatchFn(ZL_Graph* graph, ZL_Edge* inputEdges[], size_t numInputs) noexcept;
ZL_GraphID registerGraph(Compressor& compressor);
std::unique_ptr<Compressor> createCompressorFromSerialized(poly::string_view serialized);

}  // namespace tracezl
