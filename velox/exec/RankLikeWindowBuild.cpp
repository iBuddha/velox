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

#include "velox/exec/RankLikeWindowBuild.h"

namespace facebook::velox::exec {

RankLikeWindowBuild::RankLikeWindowBuild(
    const std::shared_ptr<const core::WindowNode>& windowNode,
    velox::memory::MemoryPool* pool,
    const common::SpillConfig* spillConfig,
    tsan_atomic<bool>* nonReclaimableSection)
    : WindowBuild(windowNode, pool, spillConfig, nonReclaimableSection) {}

void RankLikeWindowBuild::buildNextInputOrPartition(bool isFinished) {
  sortedRows_.push_back(inputRows_);
  if (windowPartitions_.size() <= inputCurrentPartition_) {
    auto partition =
        folly::Range(sortedRows_.back().data(), sortedRows_.back().size());

    windowPartitions_.push_back(std::make_shared<WindowPartition>(
        data_.get(), partition, inputColumns_, sortKeyInfo_, true));
  } else {
    windowPartitions_[inputCurrentPartition_]->insertNewBatch(
        sortedRows_.back());
  }

  if (isFinished) {
    windowPartitions_[inputCurrentPartition_]->setTotalNum(
        currentPartitionNum_ - 1);
    windowPartitions_[inputCurrentPartition_]->setFinished();

    inputRows_.clear();
    inputCurrentPartition_++;
    currentPartitionNum_ = 1;
  }
}

void RankLikeWindowBuild::addInput(RowVectorPtr input) {
  for (auto i = 0; i < inputChannels_.size(); ++i) {
    decodedInputVectors_[i].decode(*input->childAt(inputChannels_[i]));
  }

  for (auto row = 0; row < input->size(); ++row) {
    currentPartitionNum_++;
    char* newRow = data_->newRow();

    for (auto col = 0; col < input->childrenSize(); ++col) {
      data_->store(decodedInputVectors_[col], row, newRow, col);
    }

    if (previousRow_ != nullptr &&
        compareRowsWithKeys(previousRow_, newRow, partitionKeyInfo_)) {
      buildNextInputOrPartition(true);
    }

    inputRows_.push_back(newRow);
    previousRow_ = newRow;
  }

  buildNextInputOrPartition(false);

  inputRows_.clear();
}

void RankLikeWindowBuild::noMoreInput() {
  isFinished_ = true;
  windowPartitions_[outputCurrentPartition_]->setTotalNum(
      currentPartitionNum_ - 1);
  windowPartitions_[outputCurrentPartition_]->setFinished();
  inputRows_.clear();
}

std::shared_ptr<WindowPartition> RankLikeWindowBuild::nextPartition() {
  outputCurrentPartition_++;
  return windowPartitions_[outputCurrentPartition_];
}

bool RankLikeWindowBuild::hasNextPartition() {
  return windowPartitions_.size() > 0 &&
      outputCurrentPartition_ != windowPartitions_.size() - 1;
}

} // namespace facebook::velox::exec
