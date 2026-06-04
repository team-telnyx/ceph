// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include "include/common_fwd.h"

#include <optional>
#include <string>

class DoutPrefixProvider;
struct RGWDataSyncCtx;
struct rgw_bucket_shard;

namespace rgw::sync_observability {

struct Event {
  std::string realm;
  std::string zonegroup;
  std::string source_zone;
  std::string dest_zone;
  std::string metric;
  std::string sync_type = "unknown";
  std::string phase = "unknown";
  std::string result = "unknown";
  std::string error = "unknown";
  std::string remote_op;
  std::string operation;
  std::string op_state;
  std::string bilog_op;
  std::string op_id;
  std::string op_tag;
  std::string object;
  std::string object_instance;
  std::string versioned;
  std::string null_verid;
  std::string bilog_flags;
  std::string owner;
  std::string owner_display_name;
  std::string failure_stage;
  std::string reason;
  std::string remote_id;
  std::string endpoint_event;
  std::string endpoint_hash;
  std::string bucket;
  std::string bucket_id;
  std::optional<int> endpoint_count;
  std::optional<int> unavailable_count;
  std::optional<int> data_shard;
  std::optional<int> bucket_shard;
  std::optional<int> shard;
  std::optional<double> unavailable_age_seconds;
  std::optional<double> duration_seconds;
  std::optional<double> value;
};

bool enabled(CephContext *cct);
bool bucket_debug_enabled(CephContext *cct, std::string_view bucket);
std::string result_label(int ret);
std::string error_label(int ret);
std::string reason_label(int ret);
void emit(const DoutPrefixProvider *dpp, RGWDataSyncCtx *sc, Event event);
void emit(const DoutPrefixProvider *dpp, CephContext *cct, Event event);
void add_data_shard(Event *event, int shard_id);
void add_bucket(Event *event, const rgw_bucket_shard& bs);
void add_debug_bucket(Event *event, CephContext *cct, const rgw_bucket_shard& bs);

} // namespace rgw::sync_observability
