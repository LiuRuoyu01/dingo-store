// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "br/backup_sql_data.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "common/constant.h"
#include "common/helper.h"
#include "fmt/core.h"
#include "proto/common.pb.h"

namespace br {

#ifndef ENABLE_BACKUP_SQL_DATA_PTHREAD
#define ENABLE_BACKUP_SQL_DATA_PTHREAD
#endif

// #undef ENABLE_BACKUP_SQL_DATA_PTHREAD

BackupSqlData::BackupSqlData(ServerInteractionPtr coordinator_interaction, ServerInteractionPtr store_interaction,
                             ServerInteractionPtr index_interaction, ServerInteractionPtr document_interaction,
                             const std::string& backupts, int64_t backuptso_internal, const std::string& storage,
                             const std::string& storage_internal)
    : BackupDataBase(coordinator_interaction, store_interaction, index_interaction, document_interaction, backupts,
                     backuptso_internal, storage, storage_internal, dingodb::Constant::kSqlData) {}

BackupSqlData::~BackupSqlData() = default;

std::shared_ptr<BackupSqlData> BackupSqlData::GetSelf() { return shared_from_this(); }

butil::Status BackupSqlData::Filter() {
  if (!wait_for_handle_store_regions_) {
    wait_for_handle_store_regions_ = std::make_shared<std::vector<dingodb::pb::common::Region>>();
  }

  if (!wait_for_handle_index_regions_) {
    wait_for_handle_index_regions_ = std::make_shared<std::vector<dingodb::pb::common::Region>>();
  }

  if (!wait_for_handle_document_regions_) {
    wait_for_handle_document_regions_ = std::make_shared<std::vector<dingodb::pb::common::Region>>();
  }

  for (const auto& region : region_map_->regions()) {
    // only handle executor txn region
    // remove meta(remove_region_list_) region from region_map_
    if (dingodb::Helper::IsExecutorTxn(region.definition().range().start_key())) {
      auto iter = std::find(remove_region_list_.begin(), remove_region_list_.end(), region.id());
      if (iter == remove_region_list_.end()) {
        if (dingodb::pb::common::RegionType::STORE_REGION == region.region_type()) {
          wait_for_handle_store_regions_->push_back(region);
        } else if (dingodb::pb::common::RegionType::INDEX_REGION == region.region_type()) {
          wait_for_handle_index_regions_->push_back(region);
        } else if (dingodb::pb::common::RegionType::DOCUMENT_REGION == region.region_type()) {
          wait_for_handle_document_regions_->push_back(region);
        }
      }
    }
  }

  if (FLAGS_br_log_switch_backup_detail) {
    DINGO_LOG(INFO) << "sql data : wait_for_handle_store_regions size = " << wait_for_handle_store_regions_->size();
    int i = 0;
    std::string s;
    for (const auto& region : *wait_for_handle_store_regions_) {
      if (0 != i++) {
        s += ", ";
      }
      s += std::to_string(region.id());
      if (i == 10) {
        DINGO_LOG(INFO) << "sql data : wait_for_handle_store_regions region id=[" << s << "]";
        s.clear();
        i = 0;
      }
    }

    if (!s.empty()) {
      DINGO_LOG(INFO) << "sql data : wait_for_handle_store_regions region id=[" << s << "]";
    }

    s.clear();
    i = 0;

    DINGO_LOG(INFO) << "sql data : wait_for_handle_index_regions size = " << wait_for_handle_index_regions_->size();
    for (const auto& region : *wait_for_handle_index_regions_) {
      if (0 != i++) {
        s += ", ";
      }
      s += std::to_string(region.id());
      if (i == 10) {
        DINGO_LOG(INFO) << "sql data : wait_for_handle_index_regions region id=[" << s << "]";
        s.clear();
        i = 0;
      }
    }

    if (!s.empty()) {
      DINGO_LOG(INFO) << "sql data : wait_for_handle_store_regions region id=[" << s << "]";
    }

    s.clear();
    i = 0;

    DINGO_LOG(INFO) << "sql data : wait_for_handle_document_regions size = "
                    << wait_for_handle_document_regions_->size();
    for (const auto& region : *wait_for_handle_document_regions_) {
      if (0 != i++) {
        s += ", ";
      }
      s += std::to_string(region.id());
      if (i == 10) {
        DINGO_LOG(INFO) << "sql data : wait_for_handle_document_regions region id=[" << s << "]";
        s.clear();
        i = 0;
      }
    }

    if (!s.empty()) {
      DINGO_LOG(INFO) << "sql data : wait_for_handle_document_regions region id=[" << s << "]";
    }
  }

  return butil::Status::OK();
}

butil::Status BackupSqlData::RemoveSqlMeta(const std::vector<int64_t>& meta_region_list) {
  remove_region_list_ = meta_region_list;
  return butil::Status::OK();
}

butil::Status BackupSqlData::Run() {
  butil::Status status;
  if (!save_store_region_map_) {
    save_store_region_map_ =
        std::make_shared<std::map<int64_t, dingodb::pb::common::BackupDataFileValueSstMetaGroup>>();
  }

  if (!save_index_region_map_) {
    save_index_region_map_ =
        std::make_shared<std::map<int64_t, dingodb::pb::common::BackupDataFileValueSstMetaGroup>>();
  }

  if (!save_document_region_map_) {
    save_document_region_map_ =
        std::make_shared<std::map<int64_t, dingodb::pb::common::BackupDataFileValueSstMetaGroup>>();
  }

  int64_t total_regions_count = wait_for_handle_store_regions_->size() + wait_for_handle_index_regions_->size() +
                                wait_for_handle_document_regions_->size();

  // store
  status = DoAsyncBackupRegion(store_interaction_, "StoreService", wait_for_handle_store_regions_,
                               already_handle_store_regions_, save_store_region_map_);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << status.error_cstr();
    return status;
  }

  // index
  status = DoAsyncBackupRegion(index_interaction_, "IndexService", wait_for_handle_index_regions_,
                               already_handle_index_regions_, save_index_region_map_);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << status.error_cstr();
    return status;
  }

  // document
  status = DoAsyncBackupRegion(document_interaction_, "DocumentService", wait_for_handle_document_regions_,
                               already_handle_document_regions_, save_document_region_map_);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << status.error_cstr();
    return status;
  }

  std::atomic<int64_t> last_already_handle_regions = 0;
  std::cerr << "Full Backup Sql Data " << "<";
  DINGO_LOG(INFO) << "Full Backup Sql Data " << "<";
  std::string s;
  while (!is_need_exit_) {
    already_handle_regions_ =
        already_handle_store_regions_ + already_handle_index_regions_ + already_handle_document_regions_;

    int64_t diff = already_handle_regions_ - last_already_handle_regions;
    for (int i = 0; i < diff; i++) {
      std::cerr << "-";
      s += "-";
    }

    if (already_handle_regions_ >= total_regions_count) {
      break;
    }

    last_already_handle_regions.store(already_handle_regions_);

    sleep(1);
  }

  if (is_need_exit_) {
    return last_error_;
  }

  std::cerr << ">" << " 100.00%" << " [" << "S:" << wait_for_handle_store_regions_->size()
            << ",I:" << wait_for_handle_index_regions_->size() << ",D:" << wait_for_handle_document_regions_->size()
            << "]";
  DINGO_LOG(INFO) << s;
  DINGO_LOG(INFO) << ">" << " 100.00%" << " [" << "S:" << wait_for_handle_store_regions_->size()
                  << ",I:" << wait_for_handle_index_regions_->size()
                  << ",D:" << wait_for_handle_document_regions_->size() << "]";

  std::cout << std::endl;

  DINGO_LOG(INFO) << "backup sql data  " << "total_regions : " << already_handle_regions_
                  << ", store_regions : " << already_handle_store_regions_
                  << ", index_regions : " << already_handle_index_regions_
                  << ", document_regions : " << already_handle_document_regions_;

  return butil::Status::OK();
}

butil::Status BackupSqlData::DoAsyncBackupRegion(
    ServerInteractionPtr interaction, const std::string& service_name,
    std::shared_ptr<std::vector<dingodb::pb::common::Region>> wait_for_handle_regions,
    std::atomic<int64_t>& already_handle_regions,
    std::shared_ptr<std::map<int64_t, dingodb::pb::common::BackupDataFileValueSstMetaGroup>> save_region_map) {
  std::shared_ptr<BackupSqlData> self = GetSelf();
  auto lambda_call = [self, interaction, service_name, wait_for_handle_regions, &already_handle_regions,
                      save_region_map]() {
    self->DoBackupRegionInternal(interaction, service_name, wait_for_handle_regions, already_handle_regions,
                                 save_region_map);
  };

#if defined(ENABLE_BACKUP_SQL_DATA_PTHREAD)
  std::thread th(lambda_call);
  th.detach();
#else

  std::function<void()>* call = new std::function<void()>;
  *call = lambda_call;
  bthread_t th;

  int ret = bthread_start_background(
      &th, nullptr,
      [](void* arg) -> void* {
        auto* call = static_cast<std::function<void()>*>(arg);
        (*call)();
        delete call;
        return nullptr;
      },
      call);
  if (ret != 0) {
    DINGO_LOG(ERROR) << fmt::format("bthread_start_background fail");
    return butil::Status(dingodb::pb::error::EINTERNAL, "bthread_start_background fail");
  }
#endif  // #if defined(ENABLE_BACKUP_SQL_DATA_PTHREAD)

  return butil::Status::OK();
}

}  // namespace br