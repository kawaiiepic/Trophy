/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "capture/IVideoCaptureSource.h"

extern "C" {
#include <libavutil/hwcontext_drm.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sys/stat.h>
#include <unistd.h>

namespace TrophyBackend {

// FFmpeg's VA-API DRM_PRIME mapper supports only ONE DRM object. One
// multi-plane GBM allocation can have several exported plane FDs, but those
// FDs must identify the SAME allocation to be represented by one object.
// Never coalesce distinct GPU buffers: that would silently corrupt frames.
enum class VaapiDrmDescriptorStatus {
  Ok,
  InvalidPlane,
  SeparateObjects,
  DuplicateFailed,
};

inline VaapiDrmDescriptorStatus buildVaapiDrmDescriptor(
    const VideoFrame &frame, AVDRMFrameDescriptor &descriptor) {
  const size_t count =
      frame.dmaBufPlanes.empty() ? size_t(1) : frame.dmaBufPlanes.size();
  if (!frame.isDmaBuf || frame.dmaBufFd < 0 || frame.width <= 0 ||
      frame.height <= 0 || frame.drmFormat == 0 ||
      count == 0 || count > AV_DRM_MAX_PLANES)
    return VaapiDrmDescriptorStatus::InvalidPlane;

  // Compare actual kernel allocation identities, not integer FD numbers:
  // gbm_bo_get_fd_for_plane() returns a new FD for each plane.
  const int primaryFd = frame.dmaBufPlanes.empty()
                            ? frame.dmaBufFd : frame.dmaBufPlanes[0].fd;
  struct stat primary{};
  if (primaryFd < 0 || fstat(primaryFd, &primary) != 0)
    return VaapiDrmDescriptorStatus::InvalidPlane;

  size_t fallbackSize = 0;
  for (size_t index = 0; index < count; ++index) {
    const DmaBufPlane plane = frame.dmaBufPlanes.empty()
        ? DmaBufPlane{frame.dmaBufFd, frame.dmaBufStride,
                      static_cast<uint32_t>(frame.dmaBufOffset), 0}
        : frame.dmaBufPlanes[index];
    if (plane.fd < 0 || plane.stride <= 0)
      return VaapiDrmDescriptorStatus::InvalidPlane;
    struct stat actual{};
    if (fstat(plane.fd, &actual) != 0)
      return VaapiDrmDescriptorStatus::InvalidPlane;
    if (actual.st_dev != primary.st_dev || actual.st_ino != primary.st_ino)
      return VaapiDrmDescriptorStatus::SeparateObjects;

    const size_t rows = index == 0 ? size_t(frame.height) : size_t(1);
    const size_t need = size_t(plane.offset) + size_t(plane.stride) * rows;
    fallbackSize = std::max(fallbackSize, size_t(plane.size));
    fallbackSize = std::max(fallbackSize, need);
  }

  const int owned = ::dup(primaryFd);
  if (owned < 0)
    return VaapiDrmDescriptorStatus::DuplicateFailed;

  descriptor = {};
  descriptor.nb_objects = 1;
  descriptor.objects[0].fd = owned;
  descriptor.objects[0].format_modifier = frame.drmModifier;
  // DMA-BUF SEEK_END returns the actual allocation size on supporting
  // kernels. PipeWire's producer can leave spa_data.maxsize == 0.
  const off_t end = lseek(owned, 0, SEEK_END);
  descriptor.objects[0].size = end > 0 ? size_t(end) : fallbackSize;
  descriptor.nb_layers = 1;
  descriptor.layers[0].format = frame.drmFormat;
  descriptor.layers[0].nb_planes = static_cast<int>(count);
  for (size_t index = 0; index < count; ++index) {
    const DmaBufPlane plane = frame.dmaBufPlanes.empty()
        ? DmaBufPlane{frame.dmaBufFd, frame.dmaBufStride,
                      static_cast<uint32_t>(frame.dmaBufOffset), 0}
        : frame.dmaBufPlanes[index];
    auto &layout = descriptor.layers[0].planes[index];
    layout.object_index = 0;
    layout.pitch = plane.stride;
    layout.offset = plane.offset;
  }
  return VaapiDrmDescriptorStatus::Ok;
}

inline const char *vaapiDrmDescriptorError(VaapiDrmDescriptorStatus status) {
  switch (status) {
    case VaapiDrmDescriptorStatus::Ok: return "success";
    case VaapiDrmDescriptorStatus::InvalidPlane:
      return "invalid DRM PRIME plane metadata";
    case VaapiDrmDescriptorStatus::SeparateObjects:
      return "planes reference distinct GPU allocations; VA-API cannot map multiple DRM objects";
    case VaapiDrmDescriptorStatus::DuplicateFailed:
      return "failed to duplicate DRM PRIME allocation fd";
  }
  return "unknown DRM PRIME descriptor error";
}

} // namespace TrophyBackend
