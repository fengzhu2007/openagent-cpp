#include "provider/cost.h"
#include <algorithm>

namespace CostCalculator {

const std::vector<ModelPricing> &getPricingTable()
{
    // Pricing as of 2024-2025 (per 1M tokens, in USD)
    // contextUpTo=0 means "default tier" (up to the first threshold)
    static const std::vector<ModelPricing> table = {
        // --- Claude 3.5 Sonnet ---
        {"claude-3-5-sonnet", {
            {200000, 3.0, 15.0, 0.30, 3.75},
            {0, 0, 0, 0, 0}  // no higher tier
        }},
        // --- Claude 3.5 Haiku ---
        {"claude-3-5-haiku", {
            {200000, 0.80, 4.0, 0.08, 1.0},
            {0, 0, 0, 0, 0}
        }},
        // --- Claude 3 Opus ---
        {"claude-3-opus", {
            {200000, 15.0, 75.0, 1.50, 18.75},
            {0, 0, 0, 0, 0}
        }},
        // --- Claude 4 / Sonnet 4 ---
        {"claude-sonnet-4", {
            {200000, 3.0, 15.0, 0.30, 3.75},
            {0, 0, 0, 0, 0}
        }},
        {"claude-4", {
            {200000, 3.0, 15.0, 0.30, 3.75},
            {0, 0, 0, 0, 0}
        }},
        // --- GPT-4o ---
        {"gpt-4o", {
            {0, 2.50, 10.0, 1.25, 2.50},
            {0, 0, 0, 0, 0}
        }},
        // --- GPT-4o-mini ---
        {"gpt-4o-mini", {
            {0, 0.15, 0.60, 0.07, 0.15},
            {0, 0, 0, 0, 0}
        }},
        // --- o1 ---
        {"o1", {
            {0, 15.0, 60.0, 7.50, 15.0},
            {0, 0, 0, 0, 0}
        }},
        // --- o1-mini ---
        {"o1-mini", {
            {0, 3.0, 12.0, 1.50, 3.0},
            {0, 0, 0, 0, 0}
        }},
        // --- o3 ---
        {"o3", {
            {0, 10.0, 40.0, 2.50, 10.0},
            {0, 0, 0, 0, 0}
        }},
        // --- o3-mini ---
        {"o3-mini", {
            {0, 1.10, 4.40, 0.55, 1.10},
            {0, 0, 0, 0, 0}
        }},
        // --- o4-mini ---
        {"o4-mini", {
            {0, 1.10, 4.40, 0.275, 1.10},
            {0, 0, 0, 0, 0}
        }},
        // --- Gemini 2.5 Pro ---
        {"gemini-2.5-pro", {
            {200000, 1.25, 10.0, 0.315, 1.25},
            {0, 2.50, 15.0, 0.63, 2.50}
        }},
        // --- Gemini 2.5 Flash ---
        {"gemini-2.5-flash", {
            {200000, 0.15, 0.60, 0.07, 0.15},
            {0, 0.30, 2.50, 0.07, 0.30}
        }},
        // --- DeepSeek V3 ---
        {"deepseek-chat", {
            {0, 0.27, 1.10, 0.07, 0.27},
            {0, 0, 0, 0, 0}
        }},
        // --- DeepSeek R1 ---
        {"deepseek-reasoner", {
            {0, 0.55, 2.19, 0.14, 0.55},
            {0, 0, 0, 0, 0}
        }},
    };
    return table;
}

const ModelPricing *findPricing(const std::string &modelId)
{
    const auto &table = getPricingTable();

    // Exact match first
    for (const auto &entry : table) {
        if (modelId == entry.modelId) {
            return &entry;
        }
    }

    // Prefix match (e.g. "claude-3-5-sonnet-20241022" matches "claude-3-5-sonnet")
    for (const auto &entry : table) {
        if (modelId.size() >= entry.modelId.size() &&
            modelId.substr(0, entry.modelId.size()) == entry.modelId) {
            return &entry;
        }
    }

    return nullptr;
}

double calculateCost(const std::string &modelId,
                     int64_t inputTokens, int64_t outputTokens,
                     int64_t cacheRead, int64_t cacheWrite,
                     int64_t reasoningTokens)
{
    const ModelPricing *pricing = findPricing(modelId);
    if (!pricing || pricing->tiers.empty()) {
        return 0.0;
    }

    // Select tier based on total input context size
    const PricingTier *tier = &pricing->tiers[0];
    int64_t totalContext = inputTokens + cacheRead + cacheWrite;
    for (const auto &t : pricing->tiers) {
        if (t.contextUpTo == 0) {
            // Default/fallback tier
            if (&t == &pricing->tiers[0]) tier = &t;
            break;
        }
        if (totalContext <= t.contextUpTo) {
            tier = &t;
            break;
        }
        tier = &t;  // Use the highest tier if all exceeded
    }

    double cost = 0.0;
    cost += (static_cast<double>(inputTokens) / 1000000.0) * tier->inputPerM;
    cost += (static_cast<double>(outputTokens) / 1000000.0) * tier->outputPerM;
    cost += (static_cast<double>(cacheRead) / 1000000.0) * tier->cacheReadPerM;
    cost += (static_cast<double>(cacheWrite) / 1000000.0) * tier->cacheWritePerM;
    // Reasoning tokens are typically charged at output rate
    cost += (static_cast<double>(reasoningTokens) / 1000000.0) * tier->outputPerM;

    return cost;
}

} // namespace CostCalculator
