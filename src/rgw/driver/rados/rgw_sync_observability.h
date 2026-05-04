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
  std::string metric;
  std::string sync_type = "unknown";
  std::string phase = "unknown";
  std::string result = "unknown";
  std::string error = "unknown";
  std::string remote_op;
  std::string bucket;
  std::string bucket_id;
  std::optional<int> shard;
  std::optional<double> duration_seconds;
  std::optional<double> value;
};

bool enabled(CephContext *cct);
std::string result_label(int ret);
std::string error_label(int ret);
void emit(const DoutPrefixProvider *dpp, RGWDataSyncCtx *sc, Event event);
void add_debug_bucket(Event *event, CephContext *cct, const rgw_bucket_shard& bs);

} // namespace rgw::sync_observability
