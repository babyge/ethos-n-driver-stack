//
// Copyright © 2018-2020 Arm Limited. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//

#include "StrategyX.hpp"

#include "SramAllocator.hpp"
#include "StrategiesCommon.hpp"
#include "Utils.hpp"

#include <ethosn_command_stream/CommandData.hpp>

namespace ethosn
{
namespace support_library
{

using namespace utils;

namespace
{

enum class WeightsReloadingOptions
{
    NO_RELOADING,
    RELOADING_DOUBLE_BUFFERING,
    RELOADING_NO_DOUBLE_BUFFERING,
};

bool IsUpsampling(command_stream::UpsampleType upsampleType)
{
    return upsampleType != command_stream::UpsampleType::OFF;
}

bool IsFullyConnected(command_stream::MceOperation mceOperation)
{
    return mceOperation == command_stream::MceOperation::FULLY_CONNECTED;
}

bool IsBlockConfigCompatible(const command_stream::BlockConfig& blockConfig,
                             const HardwareCapabilities& capabilities,
                             command_stream::MceOperation mceOperation,
                             command_stream::UpsampleType upsampleType)
{
    const uint32_t numAccumulatorsPerOg     = capabilities.GetTotalAccumulatorsPerEngine();
    const uint32_t currBlockWidth           = blockConfig.m_BlockWidth();
    const uint32_t currBlockHeight          = blockConfig.m_BlockHeight();
    const uint32_t numberOfElementsInABlock = currBlockWidth * currBlockHeight;

    bool isUpsampling     = IsUpsampling(upsampleType);
    bool isFullyConnected = IsFullyConnected(mceOperation);

    if (numberOfElementsInABlock > numAccumulatorsPerOg)
    {
        return false;
    }

    if (isFullyConnected && (currBlockWidth != 8U || currBlockHeight != 8U))
    {
        return false;
    }

    // When using upsampling, we need to have a block size of
    // 16x16 because the input tensor is DMA using the size
    // "BlockSizeW/2 X BlockSizeH/2" and the DMA cannot transfer block
    // smaller than 8x8
    if (isUpsampling && (currBlockWidth != 16U || currBlockHeight != 16U))
    {
        return false;
    }

    return true;
}

// Given a requested shape for the output stripe calculates what the actual stripe sizes would be
// (accounting for hardware and firmware constraints)
// and what the tile sizes would be (accounting for buffering etc.) and checks if all this would
// fit into SRAM.
bool TryStripeShapes(const command_stream::MceOperation& mceOperation,
                     SramAllocator& sramAllocator,
                     const TensorShape& requestedOutputStripe,
                     const uint32_t requestedInputChannels,
                     const TensorShape& inputShape,
                     const TensorShape& outputShape,
                     const DataFormat weightsFormat,
                     const TensorShape& weightsShape,
                     const HardwareCapabilities& capabilities,
                     const utils::ShapeMultiplier& mceShapeMultiplier,
                     const utils::ShapeMultiplier& pleShapeMultiplier,
                     std::pair<const uint32_t, const uint32_t> pad,
                     std::pair<const bool, const uint32_t> inputStaticAndOffset,
                     TensorConfig& outTensorConfig,
                     const uint32_t depthMax,
                     const bool allowInputBuffering                 = false,
                     const bool avoidInputReloading                 = false,
                     const bool activationCompression               = false,
                     const WeightsReloadingOptions weightsReloading = WeightsReloadingOptions::NO_RELOADING)
{
    const bool isFullyConnected = (mceOperation == command_stream::MceOperation::FULLY_CONNECTED);
    const bool isHwio           = (weightsFormat == DataFormat::HWIO);

    const uint32_t brickGroupHeight               = GetHeight(capabilities.GetBrickGroupShape());
    const uint32_t brickGroupWidth                = GetWidth(capabilities.GetBrickGroupShape());
    const uint32_t brickGroupChannels             = GetChannels(capabilities.GetBrickGroupShape());
    const utils::ShapeMultiplier& shapeMultiplier = mceShapeMultiplier * pleShapeMultiplier;

    // Allow output stripe width smaller then brickGroupHeight. This is going to be fixed later to make it DMA-able when pooling is supported.
    const uint32_t outputStripeWidthMin = brickGroupWidth * shapeMultiplier.m_W;
    const uint32_t outputStripeWidthMax = RoundUpToNearestMultiple(GetWidth(outputShape), brickGroupWidth);
    uint32_t outputStripeWidth =
        std::min(RoundUpToNearestMultiple(GetWidth(requestedOutputStripe), outputStripeWidthMin), outputStripeWidthMax);

    // Allow output stripe height smaller then brickGroupHeight. This is going to be fixed later to make it DMA-able when pooling is supported.
    const uint32_t outputStripeHeightMin = brickGroupHeight * shapeMultiplier.m_H;
    const uint32_t outputStripeHeightMax = RoundUpToNearestMultiple(GetHeight(outputShape), brickGroupHeight);
    uint32_t outputStripeHeight          = std::min(
        RoundUpToNearestMultiple(GetHeight(requestedOutputStripe), outputStripeHeightMin), outputStripeHeightMax);
    // The stripe depth must be a multiple of the number of srams as this is required by the firmware and
    // PLE supports.
    // The stripe depth must also be such that no stripes may start on channels that aren't a multiple of 16 and pass
    // through into the next 16, which is not supported by the DMA (e.g. a stripe starting on channel 24
    // and going to channel 48).
    // Ensure that rounding to a multiple of brickGroupChannels is ALSO a multiple of num SRAMS
    uint32_t outputStripeChannels =
        (DivRoundUp(GetChannels(outputShape), GetChannels(requestedOutputStripe)) > 1 &&
         GetChannels(requestedOutputStripe) > brickGroupChannels * shapeMultiplier.m_C)
            ? RoundUpToNearestMultiple(GetChannels(requestedOutputStripe), brickGroupChannels * shapeMultiplier.m_C)
            : RoundUpToNearestMultiple(GetChannels(requestedOutputStripe),
                                       capabilities.GetNumberOfSrams() * shapeMultiplier.m_C);

    // Calculate input stripe from output stripe
    const uint32_t inputStripeHeightPre =
        AccountForFullDimension(GetHeight(outputShape), GetHeight(inputShape), outputStripeHeight, shapeMultiplier.m_H);
    const uint32_t inputStripeHeight =
        RoundUpToNearestMultiple(std::min(inputStripeHeightPre, GetHeight(inputShape)), brickGroupHeight);

    const uint32_t inputStripeWidthPre =
        AccountForFullDimension(GetWidth(outputShape), GetWidth(inputShape), outputStripeWidth, shapeMultiplier.m_W);
    const uint32_t inputStripeWidth =
        RoundUpToNearestMultiple(std::min(inputStripeWidthPre, GetWidth(inputShape)), brickGroupWidth);

    // Output stripe depth maximum is set for MAXPOOLING_3x3/(2,2)
    // so that the PLE can manage spilling if the number of stripes is more than 1.
    if (utils::DivRoundUp(GetHeight(inputShape), inputStripeHeight) > 1)
    {
        outputStripeChannels = std::min(outputStripeChannels, depthMax);
    }

    // MCE output stripe shape = requestedOutputStripe / PleShapeMultiplier
    const TensorShape mceOutputStripe = { 1, outputStripeHeight / pleShapeMultiplier.m_H,
                                          outputStripeWidth / pleShapeMultiplier.m_W,
                                          outputStripeChannels / pleShapeMultiplier.m_C };

    uint32_t strideSize =
        utils::DivRoundUp(utils::RoundUpToNearestMultiple(GetChannels(inputShape), capabilities.GetNumberOfSrams()),
                          utils::RoundUpToNearestMultiple(weightsShape[2], capabilities.GetNumberOfSrams()));

    // Same considerations done above for the outputStripeChannels.
    // The difference is that the input channels need to account the stride size
    // since all the de-interleaved input channels  need to go together.
    const uint32_t inputStripeChannels =
        (DivRoundUp(GetChannels(inputShape), requestedInputChannels) > 1 &&
         requestedInputChannels > brickGroupChannels * strideSize)
            ? RoundUpToNearestMultiple(requestedInputChannels, brickGroupChannels * strideSize)
            : RoundUpToNearestMultiple(requestedInputChannels, capabilities.GetNumberOfSrams() * strideSize);

    const TensorShape inputStripe = { 1, inputStripeHeight, inputStripeWidth, inputStripeChannels };

    // Make sure that input is DMA-able.
    if ((GetHeight(inputStripe) % brickGroupHeight != 0) || (GetWidth(inputStripe) % brickGroupWidth != 0))
    {
        return false;
    }

    // Calculate weight stripe from output stripe.
    TensorShape weightStripe;
    if (isHwio)
    {
        const uint32_t weightStripeChannels =
            isFullyConnected
                ? (RoundUpToNearestMultiple(GetHeight(inputStripe) * GetWidth(inputStripe) * GetChannels(inputStripe),
                                            g_WeightsChannelVecProd))
                : GetChannels(inputStripe);

        weightStripe = { weightsShape[0], weightsShape[1], weightStripeChannels, GetChannels(mceOutputStripe) };
    }
    else
    {
        // Weight tensor must be HWIO
        assert(false);
    }

    // Work out the tile sizes by deciding how many stripes we want in each tile

    const NeedBoundary needBoundaryY = GetBoundaryRequirements(pad.first, GetHeight(inputShape), GetHeight(inputStripe),
                                                               GetHeight(mceOutputStripe), weightsShape[0]);

    const bool needsBoundarySlots = needBoundaryY.m_Before || needBoundaryY.m_After;
    const uint32_t inputStripeXZ  = GetWidth(inputStripe) * GetChannels(inputStripe);

    const uint32_t boundarySlotSize = needsBoundarySlots ? (brickGroupHeight * inputStripeXZ) : 0U;
    const uint32_t defaultSlotSize  = TotalSizeBytes(inputStripe);

    const uint32_t totalSlotSize = (2U * boundarySlotSize) + defaultSlotSize;

    // Clamp this to the maximum number of stripes possible (i.e. if the image is small enough don't bother allocating
    // more space than we could use).
    const uint32_t numInputStripesTotalX = DivRoundUp(GetWidth(inputShape), GetWidth(inputStripe));
    const uint32_t numInputStripesTotalY = DivRoundUp(GetHeight(inputShape), GetHeight(inputStripe));
    const uint32_t numInputStripesTotalZ = DivRoundUp(GetChannels(inputShape), GetChannels(inputStripe));

    const NeedBoundary needBoundaryX = GetBoundaryRequirements(pad.second, GetWidth(inputShape), GetWidth(inputStripe),
                                                               GetWidth(mceOutputStripe), weightsShape[1]);

    uint32_t numInputSlots = 1U;
    numInputSlots += static_cast<uint32_t>(needBoundaryX.m_Before);
    numInputSlots += static_cast<uint32_t>(needBoundaryX.m_After);
    numInputSlots = std::min(numInputSlots, numInputStripesTotalX);

    const bool isFullHeight              = numInputStripesTotalY == 1U;
    const bool isFullWidth               = numInputStripesTotalX == 1U;
    const uint32_t numInputSlotGroupsMax = (avoidInputReloading && isFullHeight && isFullWidth)
                                               ? (numInputStripesTotalX * numInputStripesTotalY * numInputStripesTotalZ)
                                               : 2U;

    // It's better to use multiple queues if partial depth.
    const bool needSlotGroups = (GetChannels(inputShape) > GetChannels(inputStripe));
    const uint32_t inputTile =
        totalSlotSize * numInputSlots * ((allowInputBuffering && needSlotGroups) ? numInputSlotGroupsMax : 1U);

    uint32_t numWeightStripesInTile;
    if (!isFullyConnected)
    {
        if (weightsReloading == WeightsReloadingOptions::NO_RELOADING)
        {
            // First try to fit all ifm iterations in the weight tile to avoid weight reloading.
            numWeightStripesInTile = DivRoundUp(GetChannels(inputShape), GetChannels(inputStripe));
        }
        else
        {
            // If not try to weight reloading with double buffering.
            numWeightStripesInTile = weightsReloading == WeightsReloadingOptions::RELOADING_DOUBLE_BUFFERING ? 2U : 1U;
        }
    }
    else
    {
        // Fully connected: reserves two stripes for weight streaming.
        numWeightStripesInTile = 2U;
    }

    const uint32_t weightTile =
        EstimateWeightSizeBytes(weightStripe, capabilities, weightsFormat == DataFormat::HWIM) * numWeightStripesInTile;

    // To support activation compression, MCE and output stripes will need to be decoupled.
    if (activationCompression)
    {
        // Sanity check: can only consider activation compression
        // for N78 that uses FCAF formats.
        assert(capabilities.GetActivationCompressionVersion() == 1);

        // The output stripe depth must be multiple of FCAF cell depth in
        // case it gets compressed.
        // FCAF wide (HxWxC=8x16x16) is the most likely format to be used for compression.
        uint32_t minFcafDepth = 16;

        // However, FCAF deep (8x8x32) will be preferred if the tensor's height and width are both less than
        // or equal to 8.
        if (outputShape[1] <= 8 && outputShape[2] <= 8)
        {
            minFcafDepth = 32;
        }

        if (minFcafDepth > outputStripeChannels)
        {
            // If the minimum output depth for FCAF is greater than the MCE output stripe depth,
            // multiple MCE stripes would need to be accumulated to form a output stripe that
            // is deep enough for FCAF.
            outputStripeChannels = minFcafDepth;
            outputStripeHeight   = RoundUpToNearestMultiple(outputShape[1], 8);
            outputStripeWidth    = RoundUpToNearestMultiple(outputShape[2], 8);
        }
    }

    const TensorShape outputStripe = { 1, outputStripeHeight, outputStripeWidth, outputStripeChannels };

    // Make sure that output is DMA-able.
    if ((GetHeight(outputStripe) % brickGroupHeight != 0) || (GetWidth(outputStripe) % brickGroupWidth != 0))
    {
        return false;
    }

    // Outputs. We need at most 2 at a time for double-buffering.
    const uint32_t maxNumOutputStripesInTile = 2U;
    // Clamp this to the maximum number of stripes possible (i.e. if the image is small enough don't bother allocating
    // more space than we could use).
    const uint32_t numOutputStripesTotalX = DivRoundUp(GetWidth(outputShape), GetWidth(outputStripe));
    const uint32_t numOutputStripesTotalY = DivRoundUp(GetHeight(outputShape), GetHeight(outputStripe));
    const uint32_t numOutputStripesTotalZ = DivRoundUp(GetChannels(outputShape), GetChannels(outputStripe));
    const uint32_t numOutputStripesTotal  = numOutputStripesTotalX * numOutputStripesTotalY * numOutputStripesTotalZ;
    const uint32_t numOutputStripesInTile = std::min(maxNumOutputStripesInTile, numOutputStripesTotal);
    // Clamp the overall tile size to the size of the full tensor. This means that if we have a small number of stripes
    // and the last one is partial we don't waste space in the tile that will never be used.
    const uint32_t outputTileMax = TotalSizeBytes(
        TensorShape{ 1, RoundUpToNearestMultiple(GetHeight(outputShape), brickGroupHeight),
                     RoundUpToNearestMultiple(GetWidth(outputShape), brickGroupWidth),
                     RoundUpToNearestMultiple(GetChannels(outputShape), capabilities.GetNumberOfOfm()) });
    const uint32_t outputTile = std::min(TotalSizeBytes(outputStripe) * numOutputStripesInTile, outputTileMax);

    SramAllocator currentSramAllocator = sramAllocator;
    AllocationResult allocationResults =
        FitsInSram(currentSramAllocator, capabilities, inputTile, weightTile, outputTile, inputStaticAndOffset);
    if (!allocationResults.m_Success)
    {
        return false;
    }
    outTensorConfig.inputAllocation.stripeShape   = inputStripe;
    outTensorConfig.inputAllocation.tileSize      = inputTile;
    outTensorConfig.outputAllocation.stripeShape  = outputStripe;
    outTensorConfig.outputAllocation.tileSize     = outputTile;
    outTensorConfig.weightsAllocation.stripeShape = weightStripe;
    outTensorConfig.weightsAllocation.tileSize    = weightTile;
    // If we succeeded in finding a strategy, update the sram allocation state
    sramAllocator = currentSramAllocator;
    FillTensorConfigOffsets(allocationResults, outTensorConfig);
    return true;
}

// Try ZXY input traversal: streaming in Z, in X and Y and XYZ output traversal (output traversal
// matters only for the Firmware).
bool TryInputZXYOutputXYZ(const command_stream::MceOperation& mceOperation,
                          command_stream::UpsampleType upsampleType,
                          TensorConfig& tensorConfig,
                          SramAllocator& sramAllocator,
                          const TensorShape& inputShape,
                          const TensorShape& outputShape,
                          const DataFormat weightsFormat,
                          const TensorShape& weightsShape,
                          std::pair<const uint32_t, const uint32_t> pad,
                          std::vector<command_stream::BlockConfig> allowedBlockConfigs,
                          const HardwareCapabilities& capabilities,
                          const utils::ShapeMultiplier& mceShapeMultiplier,
                          const utils::ShapeMultiplier& pleShapeMultiplier,
                          std::pair<const bool, const uint32_t> inputStaticAndOffset,
                          const uint32_t depthMax)
{
    if (inputStaticAndOffset.first)
    {
        return false;
    }

    const bool isFullyConnected = IsFullyConnected(mceOperation);

    // Sort the block config (allowedBlockConfigs is a copy)
    std::sort(allowedBlockConfigs.begin(), allowedBlockConfigs.end(),
              [](command_stream::BlockConfig a, command_stream::BlockConfig b) {
                  return ((a.m_BlockWidth() > b.m_BlockWidth()) ||
                          ((a.m_BlockWidth() == b.m_BlockWidth()) && (a.m_BlockHeight() >= b.m_BlockHeight())));
              });

    struct Params
    {
        uint32_t blockHeight;
        uint32_t blockWidth;
        uint32_t inputStripeChannel;
        uint32_t outputStripeHeight;
        uint32_t outputStripeWidth;
        uint32_t outputStripeChannel;
        bool activationCompression;
    };

    std::vector<bool> activationCompressionOptions;

    // Activation compression options:
    // {true, false} --- N78 and not fully connected.
    // {false}       --- otherwise
    if (capabilities.GetActivationCompressionVersion() == 1 && !isFullyConnected)
    {
        activationCompressionOptions.push_back(true);
    }
    activationCompressionOptions.push_back(false);

    const WeightsReloadingOptions weightsReloading[] = { WeightsReloadingOptions::NO_RELOADING,
                                                         WeightsReloadingOptions::RELOADING_DOUBLE_BUFFERING,
                                                         WeightsReloadingOptions::RELOADING_NO_DOUBLE_BUFFERING };

    // Generate a list of parameters we pass to TryStripeShapes so we can see all the stripe shapes which could be attempted.
    std::vector<Params> paramsList;

    for (auto compression : activationCompressionOptions)
    {
        for (auto& currBlockConfig : allowedBlockConfigs)
        {
            if (!IsBlockConfigCompatible(currBlockConfig, capabilities, mceOperation, upsampleType))
            {
                continue;
            }

            const uint32_t currBlockWidth  = currBlockConfig.m_BlockWidth();
            const uint32_t currBlockHeight = currBlockConfig.m_BlockHeight();
            // Mce can produce a single block only.
            const uint32_t outputStripeHeight = currBlockHeight * pleShapeMultiplier.m_H;
            const uint32_t outputStripeWidth  = currBlockWidth * pleShapeMultiplier.m_W;

            for (uint32_t numInputChannelSplits = 2U; numInputChannelSplits < GetChannels(inputShape);
                 ++numInputChannelSplits)
            {
                const uint32_t inputStripeChannel  = GetChannels(inputShape) / numInputChannelSplits;
                const uint32_t outputStripeChannel = capabilities.GetNumberOfOfm() * pleShapeMultiplier.m_C;
                paramsList.push_back({ currBlockConfig.m_BlockHeight(), currBlockConfig.m_BlockWidth(),
                                       inputStripeChannel, outputStripeHeight, outputStripeWidth, outputStripeChannel,
                                       compression });
            }
        }
    }

    if (paramsList.size() == 0)
    {
        return false;
    }

    auto TryConf = [&](const Params params, const bool allowInputBuffering, const bool avoidInputReloading,
                       const WeightsReloadingOptions weightsReloading) {
        assert(!avoidInputReloading || allowInputBuffering);
        if (TryStripeShapes(mceOperation, sramAllocator,
                            { 1, params.outputStripeHeight, params.outputStripeWidth, params.outputStripeChannel },
                            params.inputStripeChannel, inputShape, outputShape, weightsFormat, weightsShape,
                            capabilities, mceShapeMultiplier, pleShapeMultiplier, pad, inputStaticAndOffset,
                            tensorConfig, depthMax, allowInputBuffering, avoidInputReloading,
                            params.activationCompression, weightsReloading))
        {
            // Check that input stripe is partial depth.
            if (GetChannels(tensorConfig.inputAllocation.stripeShape) < GetChannels(inputShape))
            {
                tensorConfig.blockWidth  = params.blockWidth;
                tensorConfig.blockHeight = params.blockHeight;
                tensorConfig.strategy    = Strategy::STRATEGY_X;
                return true;
            }
        }
        return false;
    };

    // Below it is going to try:
    // a. Fit all input stripes in the tile to avoid reloading and allow buffering
    // b. Fit at least two input stripes (including neighbouring) for double buffering
    // c. No buffering
    // with all possible weights reloading options as following:
    // a. Fit all weight stripes in the tile (NO_RELOADING)
    // b. Fit at least two weight stripes stripes (RELOADING_DOUBLE_BUFFERING)
    // c. Only single weight stripe can fit so no buffering (RELOADING_NO_DOUBLE_BUFFERING)

    for (auto& tryWeightsReloading : weightsReloading)
    {
        // a. Try all configurations using input buffering.
        for (auto params : paramsList)
        {
            if (TryConf(params, true, true, tryWeightsReloading))
            {
                return true;
            }
        }

        // b. If here it means that it cannot avoid input reloading.
        for (auto params : paramsList)
        {
            if (TryConf(params, true, false, tryWeightsReloading))
            {
                return true;
            }
        }

        // c. If here it means that it cannot do input buffering.
        for (auto params : paramsList)
        {
            if (TryConf(params, false, false, tryWeightsReloading))
            {
                return true;
            }
        }
    }

    return false;
}

// Try XY input traversal: streaming in X and Y and XYZ output traversal (output traversal)
// matters only for the Firmware).
bool TryInputXYOutputXYZ(const command_stream::MceOperation& mceOperation,
                         command_stream::UpsampleType upsampleType,
                         TensorConfig& tensorConfig,
                         SramAllocator& sramAllocator,
                         const TensorShape& inputShape,
                         const TensorShape& outputShape,
                         const DataFormat weightsFormat,
                         const TensorShape& weightsShape,
                         std::pair<const uint32_t, const uint32_t> pad,
                         std::vector<command_stream::BlockConfig> allowedBlockConfigs,
                         const HardwareCapabilities& capabilities,
                         const utils::ShapeMultiplier& mceShapeMultiplier,
                         const utils::ShapeMultiplier& pleShapeMultiplier,
                         std::pair<const bool, const uint32_t> inputStaticAndOffset,
                         const uint32_t depthMax)
{
    if (inputStaticAndOffset.first)
    {
        return false;
    }

    const bool isFullyConnected = (mceOperation == command_stream::MceOperation::FULLY_CONNECTED);

    // Allow only fully connected since this is equivalent of strategy 1 not yet fully supported and
    // tested in strategy X.
    if (!isFullyConnected)
    {
        return false;
    }

    // Sort the block config (allowedBlockConfigs is a copy)
    std::sort(allowedBlockConfigs.begin(), allowedBlockConfigs.end(),
              [](command_stream::BlockConfig a, command_stream::BlockConfig b) {
                  return ((a.m_BlockWidth() > b.m_BlockWidth()) ||
                          ((a.m_BlockWidth() == b.m_BlockWidth()) && (a.m_BlockHeight() >= b.m_BlockHeight())));
              });

    struct Params
    {
        uint32_t blockHeight;
        uint32_t blockWidth;
        uint32_t inputStripeChannel;
        uint32_t outputStripeHeight;
        uint32_t outputStripeWidth;
        uint32_t outputStripeChannel;
    };

    // Generate a list of parameters we pass to TryStripeShapes so we can see all the stripe shapes which could be attempted.
    std::vector<Params> paramsList;
    for (auto& currBlockConfig : allowedBlockConfigs)
    {
        if (!IsBlockConfigCompatible(currBlockConfig, capabilities, mceOperation, upsampleType))
        {
            continue;
        }

        const uint32_t currBlockWidth  = currBlockConfig.m_BlockWidth();
        const uint32_t currBlockHeight = currBlockConfig.m_BlockHeight();
        // Use a single block only.
        const uint32_t outputStripeHeight = currBlockHeight * pleShapeMultiplier.m_H;
        const uint32_t outputStripeWidth  = currBlockWidth * pleShapeMultiplier.m_W;

        const uint32_t inputStripeChannel  = GetChannels(inputShape);
        const uint32_t outputStripeChannel = capabilities.GetNumberOfOfm() * pleShapeMultiplier.m_C;
        paramsList.push_back({ currBlockHeight, currBlockConfig.m_BlockWidth(), inputStripeChannel, outputStripeHeight,
                               outputStripeWidth, outputStripeChannel });
    }

    if (paramsList.size() == 0)
    {
        return false;
    }

    auto TryConf = [&](const Params params, const bool allowInputBuffering) {
        if (TryStripeShapes(mceOperation, sramAllocator,
                            { 1, params.outputStripeHeight, params.outputStripeWidth, params.outputStripeChannel },
                            params.inputStripeChannel, inputShape, outputShape, weightsFormat, weightsShape,
                            capabilities, mceShapeMultiplier, pleShapeMultiplier, pad, inputStaticAndOffset,
                            tensorConfig, depthMax, allowInputBuffering))
        {
            tensorConfig.blockWidth  = params.blockWidth;
            tensorConfig.blockHeight = params.blockHeight;
            tensorConfig.strategy    = Strategy::STRATEGY_X;
            return true;
        }
        return false;
    };

    // Try all configurations using input buffering.
    for (auto params : paramsList)
    {
        if (TryConf(params, true))
        {
            return true;
        }
    }

    // If here it means that it cannot do input buffering.
    for (auto params : paramsList)
    {
        if (TryConf(params, false))
        {
            return true;
        }
    }

    return false;
}

template <typename T>
bool IsStrategyAllowed(const std::vector<IStrategy*>& strategies)
{
    for (IStrategy* s : strategies)
    {
        if (dynamic_cast<T*>(s))
        {
            return true;
        }
    }
    return false;
}

}    //namespace

bool IsStrategyX(const command_stream::MceOperation& mceOperation,
                 const TensorConfig& tensorConfig,
                 const CompilerMceAlgorithm algorithm,
                 const std::vector<IStrategy*>& allowedStrategies)
{
    const bool isSupportedMceOperation = (mceOperation == command_stream::MceOperation::CONVOLUTION) ||
                                         (mceOperation == command_stream::MceOperation::FULLY_CONNECTED);
    const bool isSupportedAlgorithm = (algorithm == CompilerMceAlgorithm::Direct);
    const bool isSupportedStrategy  = (tensorConfig.strategy == Strategy::STRATEGY_7) ||
                                     (tensorConfig.strategy == Strategy::STRATEGY_FC) ||
                                     (tensorConfig.strategy == Strategy::NONE);
    const bool isAllowedStrategy =
        (IsStrategyAllowed<Strategy7>(allowedStrategies) || IsStrategyAllowed<StrategyFc>(allowedStrategies));
    return isSupportedMceOperation && isSupportedAlgorithm && isSupportedStrategy && isAllowedStrategy;
}

bool TryStrategyX(const command_stream::MceOperation& mceOperation,
                  const command_stream::UpsampleType upsampleType,
                  TensorConfig& tensorConfig,
                  SramAllocator& sramAllocator,
                  const TensorShape& inputShape,
                  const TensorShape& outputShape,
                  const DataFormat weightsFormat,
                  const TensorShape& weightsShape,
                  std::pair<const uint32_t, const uint32_t> pad,
                  const std::vector<command_stream::BlockConfig>& allowedBlockConfigs,
                  const HardwareCapabilities& capabilities,
                  const utils::ShapeMultiplier& mceShapeMultiplier,
                  const utils::ShapeMultiplier& pleShapeMultiplier,
                  std::pair<const bool, const uint32_t> inputStaticAndOffset,
                  const uint32_t depthMax)
{
    if (TryInputXYOutputXYZ(mceOperation, upsampleType, tensorConfig, sramAllocator, inputShape, outputShape,
                            weightsFormat, weightsShape, pad, allowedBlockConfigs, capabilities, mceShapeMultiplier,
                            pleShapeMultiplier, inputStaticAndOffset, depthMax))
    {
        return true;
    }

    if (TryInputZXYOutputXYZ(mceOperation, upsampleType, tensorConfig, sramAllocator, inputShape, outputShape,
                             weightsFormat, weightsShape, pad, allowedBlockConfigs, capabilities, mceShapeMultiplier,
                             pleShapeMultiplier, inputStaticAndOffset, depthMax))
    {
        return true;
    }

    return false;
}

}    // namespace support_library
}    // namespace ethosn
