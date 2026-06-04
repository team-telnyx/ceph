// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "rgw_sync_observability.h"

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "rgw_data_sync.h"
#include "rgw_zone.h"
#include "services/svc_zone.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define dout_subsys ceph_subsys_rgw_sync

namespace rgw::sync_observability {
namespace {

std::string json_escape(std::string_view s)
{
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

void append_json_field(std::ostream& out, std::string_view name,
                       std::string_view value, bool *first)
{
  if (!*first) {
    out << ',';
  }
  *first = false;
  out << '"' << name << "\":\"" << json_escape(value) << '"';
}

void append_json_number(std::ostream& out, std::string_view name,
                        double value, bool *first)
{
  if (!*first) {
    out << ',';
  }
  *first = false;
  out << '"' << name << "\":" << value;
}

std::string zone_name_or_id(RGWDataSyncCtx *sc, const rgw_zone_id& zone_id)
{
  if (!sc || !sc->env || !sc->env->svc || !sc->env->svc->zone) {
    return zone_id.id.empty() ? "unknown" : zone_id.id;
  }

  auto zone = sc->env->svc->zone->find_zone(zone_id);
  if (zone) {
    return zone->name;
  }
  return zone_id.id.empty() ? "unknown" : zone_id.id;
}

std::string short_bucket_id(std::string_view bucket_id)
{
  if (bucket_id.empty()) {
    return "unknown";
  }
  return std::string{bucket_id.substr(0, std::min<std::size_t>(bucket_id.size(), 8))};
}

bool bucket_allowlisted(CephContext *cct, std::string_view bucket)
{
  if (!cct || bucket.empty()) {
    return false;
  }

  const auto allowlist = cct->_conf->rgw_sync_debug_bucket_allowlist;
  std::size_t pos = 0;
  while (pos < allowlist.size()) {
    const auto comma = allowlist.find(',', pos);
    const auto end = comma == std::string::npos ? allowlist.size() : comma;
    std::string_view item{allowlist.data() + pos, end - pos};
    while (!item.empty() && item.front() == ' ') {
      item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
      item.remove_suffix(1);
    }
    if (item == "*") {
      return true;
    }
    if (item == bucket) {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return false;
}

void send_datagram(const DoutPrefixProvider *dpp, CephContext *cct,
                   const std::string& payload)
{
  const auto socket_path = cct->_conf->rgw_sync_debug_prometheus_socket;
  if (socket_path.empty()) {
    return;
  }

  if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    ldpp_dout(dpp, 5) << "rgw sync observability socket path too long" << dendl;
    return;
  }

  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    ldpp_dout(dpp, 20) << "failed to create rgw sync observability socket: "
                       << cpp_strerror(errno) << dendl;
    return;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  const auto ret = sendto(fd, payload.data(), payload.size(), MSG_DONTWAIT,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0) {
    ldpp_dout(dpp, 20) << "failed to send rgw sync observability event: "
                       << cpp_strerror(errno) << dendl;
  }

  close(fd);
}

} // namespace

bool enabled(CephContext *cct)
{
  return cct && cct->_conf->rgw_sync_debug_observability;
}

bool bucket_debug_enabled(CephContext *cct, std::string_view bucket)
{
  return enabled(cct) && bucket_allowlisted(cct, bucket);
}

std::string result_label(int ret)
{
  if (ret == 0) {
    return "success";
  }
  if (ret == -ENOENT) {
    return "skipped";
  }
  if (ret == -EBUSY || ret == -EAGAIN || ret == -ECANCELED ||
      ret == -ETIMEDOUT) {
    return "retry";
  }
  if (ret < 0) {
    return "error";
  }
  return "success";
}

std::string error_label(int ret)
{
  if (ret >= 0) {
    return "none";
  }

  switch (-ret) {
  case EAGAIN:
    return "eagain";
  case EBUSY:
    return "ebusy";
  case ECANCELED:
    return "ecanceled";
  case EIO:
    return "eio";
  case ENOENT:
    return "enoent";
  case EPERM:
    return "eperm";
  case ETIMEDOUT:
    return "etimedout";
  default:
    return "errno_" + std::to_string(-ret);
  }
}

std::string reason_label(int ret)
{
  if (ret >= 0) {
    return "none";
  }

  switch (-ret) {
  case EACCES:
  case EPERM:
    return "permission_denied";
  case EAGAIN:
  case EBUSY:
  case ECANCELED:
    return "retryable";
  case ECONNREFUSED:
  case ECONNRESET:
  case EHOSTUNREACH:
  case ENETUNREACH:
  case ETIMEDOUT:
    return "transport";
  case EINVAL:
    return "invalid_argument";
  case EIO:
    return "io";
  case ENOENT:
    return "not_found";
  default:
    return "unknown";
  }
}

void add_data_shard(Event *event, int shard_id)
{
  if (!event || shard_id < 0) {
    return;
  }

  event->data_shard = shard_id;
  if (!event->shard) {
    event->shard = shard_id;
  }
}

void add_bucket(Event *event, const rgw_bucket_shard& bs)
{
  if (!event) {
    return;
  }

  event->bucket = bs.bucket.name.empty() ? "unknown" : bs.bucket.name;
  event->bucket_id = short_bucket_id(bs.bucket.bucket_id);
  if (bs.shard_id >= 0) {
    event->bucket_shard = bs.shard_id;
    event->shard = bs.shard_id;
  }
}

void add_debug_bucket(Event *event, CephContext *cct, const rgw_bucket_shard& bs)
{
  if (!event || !bucket_allowlisted(cct, bs.bucket.name)) {
    return;
  }

  add_bucket(event, bs);
}

void emit(const DoutPrefixProvider *dpp, RGWDataSyncCtx *sc, Event event)
{
  if (!sc || !sc->env || !enabled(sc->cct)) {
    return;
  }

  std::ostringstream payload;
  bool first = true;
  payload << '{';
  append_json_field(payload, "event", "rgw_sync", &first);
  append_json_field(payload, "metric", event.metric, &first);
  append_json_field(payload, "realm",
                    event.realm.empty() ? sc->env->svc->zone->get_realm().get_name() : event.realm,
                    &first);
  append_json_field(payload, "zonegroup",
                    event.zonegroup.empty() ? sc->env->svc->zone->get_zonegroup().get_name() : event.zonegroup,
                    &first);
  append_json_field(payload, "source_zone",
                    event.source_zone.empty() ? zone_name_or_id(sc, sc->source_zone) : event.source_zone,
                    &first);
  append_json_field(payload, "dest_zone",
                    event.dest_zone.empty() ? sc->env->svc->zone->zone_name() : event.dest_zone,
                    &first);
  append_json_field(payload, "sync_type", event.sync_type, &first);
  append_json_field(payload, "phase", event.phase, &first);
  append_json_field(payload, "result", event.result, &first);
  append_json_field(payload, "error", event.error, &first);

  if (!event.remote_op.empty()) {
    append_json_field(payload, "remote_op", event.remote_op, &first);
  }
  if (!event.operation.empty()) {
    append_json_field(payload, "operation", event.operation, &first);
  }
  if (!event.op_state.empty()) {
    append_json_field(payload, "op_state", event.op_state, &first);
  }
  if (!event.bilog_op.empty()) {
    append_json_field(payload, "bilog_op", event.bilog_op, &first);
  }
  if (!event.op_id.empty()) {
    append_json_field(payload, "op_id", event.op_id, &first);
  }
  if (!event.op_tag.empty()) {
    append_json_field(payload, "op_tag", event.op_tag, &first);
  }
  if (!event.object.empty()) {
    append_json_field(payload, "object", event.object, &first);
  }
  if (!event.object_instance.empty()) {
    append_json_field(payload, "object_instance", event.object_instance, &first);
  }
  if (!event.versioned.empty()) {
    append_json_field(payload, "versioned", event.versioned, &first);
  }
  if (!event.null_verid.empty()) {
    append_json_field(payload, "null_verid", event.null_verid, &first);
  }
  if (!event.bilog_flags.empty()) {
    append_json_field(payload, "bilog_flags", event.bilog_flags, &first);
  }
  if (!event.owner.empty()) {
    append_json_field(payload, "owner", event.owner, &first);
  }
  if (!event.owner_display_name.empty()) {
    append_json_field(payload, "owner_display_name", event.owner_display_name, &first);
  }
  if (!event.failure_stage.empty()) {
    append_json_field(payload, "failure_stage", event.failure_stage, &first);
  }
  if (!event.reason.empty()) {
    append_json_field(payload, "reason", event.reason, &first);
  }
  if (!event.remote_id.empty()) {
    append_json_field(payload, "remote_id", event.remote_id, &first);
  }
  if (!event.endpoint_event.empty()) {
    append_json_field(payload, "endpoint_event", event.endpoint_event, &first);
  }
  if (!event.endpoint_hash.empty()) {
    append_json_field(payload, "endpoint_hash", event.endpoint_hash, &first);
  }
  if (!event.bucket.empty()) {
    append_json_field(payload, "bucket", event.bucket, &first);
  }
  if (!event.bucket_id.empty()) {
    append_json_field(payload, "bucket_id", event.bucket_id, &first);
  }
  if (event.endpoint_count) {
    append_json_field(payload, "endpoint_count", std::to_string(*event.endpoint_count), &first);
  }
  if (event.unavailable_count) {
    append_json_field(payload, "unavailable_count", std::to_string(*event.unavailable_count), &first);
  }
  if (event.data_shard) {
    append_json_field(payload, "data_shard", std::to_string(*event.data_shard), &first);
  }
  if (event.bucket_shard) {
    append_json_field(payload, "bucket_shard", std::to_string(*event.bucket_shard), &first);
  }
  if (event.shard) {
    append_json_field(payload, "shard", std::to_string(*event.shard), &first);
  }
  if (event.unavailable_age_seconds) {
    append_json_number(payload, "unavailable_age_seconds", *event.unavailable_age_seconds, &first);
  }
  if (event.duration_seconds) {
    append_json_number(payload, "duration_seconds", *event.duration_seconds, &first);
  }
  if (event.value) {
    append_json_number(payload, "value", *event.value, &first);
  }

  payload << "}\n";
  send_datagram(dpp, sc->cct, payload.str());
}

void emit(const DoutPrefixProvider *dpp, CephContext *cct, Event event)
{
  if (!enabled(cct)) {
    return;
  }

  std::ostringstream payload;
  bool first = true;
  payload << '{';
  append_json_field(payload, "event", "rgw_sync", &first);
  append_json_field(payload, "metric", event.metric, &first);
  if (!event.realm.empty()) {
    append_json_field(payload, "realm", event.realm, &first);
  }
  if (!event.zonegroup.empty()) {
    append_json_field(payload, "zonegroup", event.zonegroup, &first);
  }
  if (!event.source_zone.empty()) {
    append_json_field(payload, "source_zone", event.source_zone, &first);
  }
  if (!event.dest_zone.empty()) {
    append_json_field(payload, "dest_zone", event.dest_zone, &first);
  }
  append_json_field(payload, "sync_type", event.sync_type, &first);
  append_json_field(payload, "phase", event.phase, &first);
  append_json_field(payload, "result", event.result, &first);
  append_json_field(payload, "error", event.error, &first);

  if (!event.remote_op.empty()) {
    append_json_field(payload, "remote_op", event.remote_op, &first);
  }
  if (!event.operation.empty()) {
    append_json_field(payload, "operation", event.operation, &first);
  }
  if (!event.op_state.empty()) {
    append_json_field(payload, "op_state", event.op_state, &first);
  }
  if (!event.bilog_op.empty()) {
    append_json_field(payload, "bilog_op", event.bilog_op, &first);
  }
  if (!event.op_id.empty()) {
    append_json_field(payload, "op_id", event.op_id, &first);
  }
  if (!event.op_tag.empty()) {
    append_json_field(payload, "op_tag", event.op_tag, &first);
  }
  if (!event.object.empty()) {
    append_json_field(payload, "object", event.object, &first);
  }
  if (!event.object_instance.empty()) {
    append_json_field(payload, "object_instance", event.object_instance, &first);
  }
  if (!event.versioned.empty()) {
    append_json_field(payload, "versioned", event.versioned, &first);
  }
  if (!event.null_verid.empty()) {
    append_json_field(payload, "null_verid", event.null_verid, &first);
  }
  if (!event.bilog_flags.empty()) {
    append_json_field(payload, "bilog_flags", event.bilog_flags, &first);
  }
  if (!event.owner.empty()) {
    append_json_field(payload, "owner", event.owner, &first);
  }
  if (!event.owner_display_name.empty()) {
    append_json_field(payload, "owner_display_name", event.owner_display_name, &first);
  }
  if (!event.failure_stage.empty()) {
    append_json_field(payload, "failure_stage", event.failure_stage, &first);
  }
  if (!event.reason.empty()) {
    append_json_field(payload, "reason", event.reason, &first);
  }
  if (!event.remote_id.empty()) {
    append_json_field(payload, "remote_id", event.remote_id, &first);
  }
  if (!event.endpoint_event.empty()) {
    append_json_field(payload, "endpoint_event", event.endpoint_event, &first);
  }
  if (!event.endpoint_hash.empty()) {
    append_json_field(payload, "endpoint_hash", event.endpoint_hash, &first);
  }
  if (event.endpoint_count) {
    append_json_field(payload, "endpoint_count", std::to_string(*event.endpoint_count), &first);
  }
  if (event.unavailable_count) {
    append_json_field(payload, "unavailable_count", std::to_string(*event.unavailable_count), &first);
  }
  if (event.unavailable_age_seconds) {
    append_json_number(payload, "unavailable_age_seconds", *event.unavailable_age_seconds, &first);
  }
  if (event.duration_seconds) {
    append_json_number(payload, "duration_seconds", *event.duration_seconds, &first);
  }
  if (event.value) {
    append_json_number(payload, "value", *event.value, &first);
  }

  payload << "}\n";
  send_datagram(dpp, cct, payload.str());
}

} // namespace rgw::sync_observability
