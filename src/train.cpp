#include "tools/training/train.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

#include "common.h"
#include "compressor.h"
#include "tools/io/InputSetBuilder.h"

// Removed using namespace directives

void train_compressor(const std::string& trace_path, const std::string& config_path,
                      size_t num_threads) {
    std::cout << "Training compressor on " << trace_path << " with " << num_threads << " threads..."
              << std::endl;

    // Prepare input
    // We use InputSetBuilder to load the file
    openzl::tools::io::InputSetBuilder builder(true);
    builder.add_path(trace_path);
    auto inputs = std::move(builder).build();

    // Prepare base compressor
    openzl::Compressor compressor;
    ZL_GraphID startGraph = tracezl::registerGraph(compressor);

    // Select starting graph
    openzl::unwrap(ZL_Compressor_selectStartingGraphID(compressor.get(), startGraph),
                   "Failed to select starting graph");

    // Train Params
    openzl::training::TrainParams params = {
        .compressorGenFunc = tracezl::createCompressorFromSerialized,
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
