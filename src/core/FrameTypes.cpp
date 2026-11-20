#include "FrameTypes.h"
#include <unistd.h>

namespace TrophyBackend {

namespace {
void releaseDescriptorFds(FrameDescriptor &frame) {
  if (!frame.dmaBufPlanes.empty()) {
    for (const DmaBufPlane &plane : frame.dmaBufPlanes)
      if (plane.fd >= 0) ::close(plane.fd);
    frame.dmaBufPlanes.clear();
  } else if (frame.fd >= 0) {
    ::close(frame.fd);
  }
  frame.fd = -1;
}
}

bool FrameDescriptor::setDmaBufPlanes(
    const std::vector<DmaBufPlane> &borrowed) {
  if (borrowed.empty() || borrowed.size() > 4)
    return false;
  std::vector<DmaBufPlane> copies;
  copies.reserve(borrowed.size());
  for (const DmaBufPlane &plane : borrowed) {
    const int duplicated = plane.fd >= 0 ? ::dup(plane.fd) : -1;
    if (duplicated < 0) {
      for (const DmaBufPlane &saved : copies) ::close(saved.fd);
      return false;
    }
    DmaBufPlane owned = plane;
    owned.fd = duplicated;
    copies.push_back(owned);
  }
  releaseDescriptorFds(*this);
  fd = copies.front().fd;
  dmaBufPlanes = std::move(copies);
  return true;
}

FrameDescriptor::~FrameDescriptor() { releaseDescriptorFds(*this); }

FrameDescriptor::FrameDescriptor(FrameDescriptor &&other) noexcept {
  fd = other.fd;
  width = other.width;
  height = other.height;
  format = other.format;
  pixelFormat = other.pixelFormat;
  drmModifier = other.drmModifier;
  dmaBufStride = other.dmaBufStride;
  dmaBufOffset = other.dmaBufOffset;
  dmaBufPlanes = std::move(other.dmaBufPlanes);
  pts = other.pts;
  bufferLease = std::move(other.bufferLease);
  requestCpuFallback = std::move(other.requestCpuFallback);
  other.fd = -1;
  other.dmaBufPlanes.clear();
}

FrameDescriptor &FrameDescriptor::operator=(FrameDescriptor &&other) noexcept {
  if (this == &other)
    return *this;
  releaseDescriptorFds(*this);
  fd = other.fd;
  width = other.width;
  height = other.height;
  format = other.format;
  pixelFormat = other.pixelFormat;
  drmModifier = other.drmModifier;
  dmaBufStride = other.dmaBufStride;
  dmaBufOffset = other.dmaBufOffset;
  dmaBufPlanes = std::move(other.dmaBufPlanes);
  pts = other.pts;
  bufferLease = std::move(other.bufferLease);
  requestCpuFallback = std::move(other.requestCpuFallback);
  other.fd = -1;
  other.dmaBufPlanes.clear();
  return *this;
}

} // namespace TrophyBackend
