#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "json.hpp"

using json = nlohmann::json;

// Pricing tier for a model (cost per million tokens)
struct PricingTier {
    int contextUpTo;          // Context size threshold (0 = default tier)
    double inputPerM;         // Input cost per 1M tokens
    double outputPerM;        // Output cost per 1M tokens
    double cacheReadPerM;     // Cache read cost per 1M tokens
    double cacheWritePerM;    // Cache write cost per 1M tokens
};

// Model pricing entry
struct ModelPricing {
    std::string modelId;      // Model identifier (or prefix pattern)
    std::vector<PricingTier> tiers;
};

// Session cost summary
struct SessionCost {
    double totalCost = 0;
    int64_t inputTokens = 0;
    int64_t outputTokens = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite = 0;
    int64_t reasoningTokens = 0;
};

namespace CostCalculator {

// Calculate cost for a single LLM call based on model and token usage
double calculateCost(const std::string &modelId,
                     int64_t inputTokens, int64_t outputTokens,
                     int64_t cacheRead = 0, int64_t cacheWrite = 0,
                     int64_t reasoningTokens = 0);

// Get the pricing table entry for a model (returns nullptr if unknown)
const ModelPricing *findPricing(const std::string &modelId);

// Build the built-in pricing table
const std::vector<ModelPricing> &getPricingTable();

} // namespace CostCalculator
