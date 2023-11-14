/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/Unnest.h"
#include "velox/common/base/Nulls.h"
#include "velox/exec/OperatorUtils.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::exec {
Unnest::Unnest(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::UnnestNode>& unnestNode)
    : Operator(
          driverCtx,
          unnestNode->outputType(),
          operatorId,
          unnestNode->id(),
          "Unnest"),
      withOrdinality_(unnestNode->withOrdinality()),
      unnestArrayOfRows_(unnestNode->unnestArrayOfRows()) {
  const auto& inputType = unnestNode->sources()[0]->outputType();
  const auto& unnestVariables = unnestNode->unnestVariables();
  for (const auto& variable : unnestVariables) {
    if (!variable->type()->isArray() && !variable->type()->isMap()) {
      VELOX_UNSUPPORTED("Unnest operator supports only ARRAY and MAP types")
    }
    unnestChannels_.push_back(inputType->getChildIdx(variable->name()));
  }

  unnestDecoded_.resize(unnestVariables.size());

  if (withOrdinality_) {
    VELOX_CHECK_EQ(
        outputType_->children().back(),
        BIGINT(),
        "Ordinality column should be BIGINT type.")
  }

  column_index_t outputChannel = 0;
  for (const auto& variable : unnestNode->replicateVariables()) {
    identityProjections_.emplace_back(
        inputType->getChildIdx(variable->name()), outputChannel++);
  }
}

void Unnest::addInput(RowVectorPtr input) {
  input_ = std::move(input);

  for (auto& child : input_->children()) {
    child->loadedVector();
  }

  auto size = input_->size();
  inputRows_.resize(size);

  // The max number of elements at each row across all unnested columns.
  maxSizes_ = allocateSizes(size, pool());
  rawMaxSizes_ = maxSizes_->asMutable<vector_size_t>();

  rawSizes_.resize(unnestChannels_.size());
  rawOffsets_.resize(unnestChannels_.size());
  rawIndices_.resize(unnestChannels_.size());

  for (auto channel = 0; channel < unnestChannels_.size(); ++channel) {
    const auto& unnestVector = input_->childAt(unnestChannels_[channel]);
    unnestDecoded_[channel].decode(*unnestVector, inputRows_);

    auto& currentDecoded = unnestDecoded_[channel];
    rawIndices_[channel] = currentDecoded.indices();

    const ArrayVector* unnestBaseArray;
    const MapVector* unnestBaseMap;
    if (unnestVector->typeKind() == TypeKind::ARRAY) {
      unnestBaseArray = currentDecoded.base()->as<ArrayVector>();
      rawSizes_[channel] = unnestBaseArray->rawSizes();
      rawOffsets_[channel] = unnestBaseArray->rawOffsets();
    } else {
      VELOX_CHECK(unnestVector->typeKind() == TypeKind::MAP);
      unnestBaseMap = currentDecoded.base()->as<MapVector>();
      rawSizes_[channel] = unnestBaseMap->rawSizes();
      rawOffsets_[channel] = unnestBaseMap->rawOffsets();
    }

    // Count max number of elements per row.
    auto currentSizes = rawSizes_[channel];
    auto currentIndices = rawIndices_[channel];
    for (auto row = 0; row < size; ++row) {
      if (!currentDecoded.isNullAt(row)) {
        auto unnestSize = currentSizes[currentIndices[row]];
        if (rawMaxSizes_[row] < unnestSize) {
          rawMaxSizes_[row] = unnestSize;
        }
      }
    }
  }
}

RowVectorPtr Unnest::getOutput() {
  if (!input_) {
    return nullptr;
  }

  const auto size = input_->size();
  const auto maxOutputSize = outputBatchRows();

  // Limit the number of input rows to keep output batch size within
  // 'maxOutputSize' if possible. Process each input row fully. Do not break
  // single row's output into multiple batches.
  vector_size_t numInput = 0;
  vector_size_t numElements = 0;
  for (auto row = nextInputRow_; row < size; ++row) {
    numElements += rawMaxSizes_[row];
    ++numInput;

    if (numElements >= maxOutputSize) {
      break;
    }
  }

  if (numElements == 0) {
    // All arrays/maps are null or empty.
    input_ = nullptr;
    nextInputRow_ = 0;
    return nullptr;
  }

  auto output = generateOutput(nextInputRow_, numInput, numElements);

  nextInputRow_ += numInput;

  if (nextInputRow_ >= size) {
    input_ = nullptr;
    nextInputRow_ = 0;
  }

  return output;
}

RowVectorPtr Unnest::generateOutput(
    vector_size_t start,
    vector_size_t size,
    vector_size_t numElements) {
  // Create "indices" buffer to repeat rows as many times as there are elements
  // in the array (or map) in unnestDecoded.
  auto repeatedIndices = allocateIndices(numElements, pool());
  auto* rawRepeatedIndices = repeatedIndices->asMutable<vector_size_t>();
  vector_size_t index = 0;
  for (auto row = start; row < start + size; ++row) {
    for (auto i = 0; i < rawMaxSizes_[row]; i++) {
      rawRepeatedIndices[index++] = row;
    }
  }

  // Wrap "replicated" columns in a dictionary using 'repeatedIndices'.
  std::vector<VectorPtr> outputs(outputType_->size());
  for (const auto& projection : identityProjections_) {
    outputs[projection.outputChannel] = wrapChild(
        numElements, repeatedIndices, input_->childAt(projection.inputChannel));
  }

  // Create unnest columns.
  vector_size_t outputsIndex = identityProjections_.size();
  for (auto channel = 0; channel < unnestChannels_.size(); ++channel) {
    auto& currentDecoded = unnestDecoded_[channel];
    auto currentSizes = rawSizes_[channel];
    auto currentOffsets = rawOffsets_[channel];
    auto currentIndices = rawIndices_[channel];

    BufferPtr elementIndices = allocateIndices(numElements, pool());
    auto* rawElementIndices = elementIndices->asMutable<vector_size_t>();

    auto nulls = allocateNulls(numElements, pool());
    auto rawNulls = nulls->asMutable<uint64_t>();

    // Make dictionary index for elements column since they may be out of order.
    index = 0;
    bool identityMapping = true;
    for (auto row = start; row < start + size; ++row) {
      auto maxSize = rawMaxSizes_[row];

      if (!currentDecoded.isNullAt(row)) {
        auto offset = currentOffsets[currentIndices[row]];
        auto unnestSize = currentSizes[currentIndices[row]];

        if (index != offset || unnestSize < maxSize) {
          identityMapping = false;
        }

        for (auto i = 0; i < unnestSize; i++) {
          rawElementIndices[index++] = offset + i;
        }

        for (auto i = unnestSize; i < maxSize; ++i) {
          bits::setNull(rawNulls, index++, true);
        }
      } else if (maxSize > 0) {
        identityMapping = false;

        for (auto i = 0; i < maxSize; ++i) {
          bits::setNull(rawNulls, index++, true);
        }
      }
    }

    if (currentDecoded.base()->typeKind() == TypeKind::ARRAY) {
      // Construct unnest column using Array elements wrapped using above
      // created dictionary.
      auto unnestBaseArray = currentDecoded.base()->as<ArrayVector>();
      auto elements = unnestBaseArray->elements();

      if (elements->typeKind() == TypeKind::ROW && unnestArrayOfRows_) {
        // In this case the array is un-nested into multiple columns, one for
        // each child type of the row.
        // The array row elements could be encoded, however. In that case, each
        // child array accessed needs to be encoded with the array row elements
        // encoding and then wrapped with the un-nest encoding for final output.
        DecodedVector rowDecoded(*elements);
        auto rowVector = rowDecoded.base()->as<RowVector>();

        auto rowIsIdentity = rowDecoded.isIdentityMapping();
        auto numRowElements = rowDecoded.size();

        for (auto j = 0; j < rowVector->childrenSize(); j++) {
          auto childVector = rowIsIdentity
              ? rowVector->childAt(j)
              : rowDecoded.wrap(
                    rowVector->childAt(j), *elements, numRowElements);

          outputs[outputsIndex++] = identityMapping
              ? childVector
              : wrapChild(numElements, elementIndices, childVector, nulls);
        }
      } else {
        outputs[outputsIndex++] = identityMapping
            ? elements
            : wrapChild(numElements, elementIndices, elements, nulls);
      }
    } else {
      // Construct two unnest columns for Map keys and values vectors wrapped
      // using above created dictionary.
      auto unnestBaseMap = currentDecoded.base()->as<MapVector>();
      outputs[outputsIndex++] = identityMapping
          ? unnestBaseMap->mapKeys()
          : wrapChild(
                numElements, elementIndices, unnestBaseMap->mapKeys(), nulls);
      outputs[outputsIndex++] = identityMapping
          ? unnestBaseMap->mapValues()
          : wrapChild(
                numElements, elementIndices, unnestBaseMap->mapValues(), nulls);
    }
  }

  if (withOrdinality_) {
    auto ordinalityVector = std::dynamic_pointer_cast<FlatVector<int64_t>>(
        BaseVector::create(BIGINT(), numElements, pool()));

    // Set the ordinality at each result row to be the index of the element in
    // the original array (or map) plus one.
    auto rawOrdinality = ordinalityVector->mutableRawValues();
    for (auto row = start; row < start + size; ++row) {
      auto maxSize = rawMaxSizes_[row];
      std::iota(rawOrdinality, rawOrdinality + maxSize, 1);
      rawOrdinality += maxSize;
    }

    // Ordinality column is always at the end.
    outputs.back() = std::move(ordinalityVector);
  }

  return std::make_shared<RowVector>(
      pool(), outputType_, BufferPtr(nullptr), numElements, std::move(outputs));
}

bool Unnest::isFinished() {
  return noMoreInput_ && input_ == nullptr;
}
} // namespace facebook::velox::exec
