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

void add_debug_bucket(Event *event, CephContext *cct, const rgw_bucket_shard& bs)
{
  if (!event || !bucket_allowlisted(cct, bs.bucket.name)) {
    return;
  }

  event->bucket = bs.bucket.name.empty() ? "unknown" : bs.bucket.name;
  event->bucket_id = short_bucket_id(bs.bucket.bucket_id);
  if (bs.shard_id >= 0) {
    event->shard = bs.shard_id;
  }
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
  append_json_field(payload, "realm", sc->env->svc->zone->get_realm().get_name(), &first);
  append_json_field(payload, "zonegroup", sc->env->svc->zone->get_zonegroup().get_name(), &first);
  append_json_field(payload, "source_zone", zone_name_or_id(sc, sc->source_zone), &first);
  append_json_field(payload, "dest_zone", sc->env->svc->zone->zone_name(), &first);
  append_json_field(payload, "sync_type", event.sync_type, &first);
  append_json_field(payload, "phase", event.phase, &first);
  append_json_field(payload, "result", event.result, &first);
  append_json_field(payload, "error", event.error, &first);

  if (!event.remote_op.empty()) {
    append_json_field(payload, "remote_op", event.remote_op, &first);
  }
  if (!event.bucket.empty()) {
    append_json_field(payload, "bucket", event.bucket, &first);
  }
  if (!event.bucket_id.empty()) {
    append_json_field(payload, "bucket_id", event.bucket_id, &first);
  }
  if (event.shard) {
    append_json_field(payload, "shard", std::to_string(*event.shard), &first);
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

} // namespace rgw::sync_observability
