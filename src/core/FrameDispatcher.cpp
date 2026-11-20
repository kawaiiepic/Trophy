#include "FrameDispatcher.h"
#include <unistd.h>
#include <algorithm>

namespace TrophyBackend {

FrameDispatcher::FrameDispatcher() = default;
FrameDispatcher::~FrameDispatcher() = default;

void FrameDispatcher::registerSink(std::shared_ptr<IFrameSink> sink) {
  std::lock_guard<std::mutex> lg(m_lock);
  m_sinks.push_back(std::move(sink));
}

void FrameDispatcher::unregisterSink(std::shared_ptr<IFrameSink> sink) {
  std::lock_guard<std::mutex> lg(m_lock);
  m_sinks.erase(std::remove(m_sinks.begin(), m_sinks.end(), sink), m_sinks.end());
}

void FrameDispatcher::dispatch(const FrameDescriptor &frame) {
  // Snapshot the sinks under lock, then dispatch without holding it:
  // tryEnqueue is arbitrary sink code (a future blocking sink would stall
  // capture, and register/unregister from a callback would self-deadlock).
  std::vector<std::shared_ptr<IFrameSink>> sinks;
  {
    std::lock_guard<std::mutex> lg(m_lock);
    sinks = m_sinks;
  }
  for (size_t i = 0; i < sinks.size(); ++i) {
    auto &sink = sinks[i];
    // Duplicate fd per sink to give ownership to the sink (cheap dup syscall)
    // The primary fd is an alias when multi-plane descriptors are present.
    // Duplicate every plane independently before handing ownership away.
    const int dupfd = frame.dmaBufPlanes.empty() && frame.fd >= 0
                          ? ::dup(frame.fd) : -1;
    if (frame.dmaBufPlanes.empty() && frame.fd >= 0 && dupfd < 0)
      continue;
    FrameDescriptor copy(dupfd, frame.width, frame.height, frame.format,
                         frame.pts, frame.pixelFormat, frame.drmModifier,
                         frame.dmaBufStride, frame.dmaBufOffset);
    if (!frame.dmaBufPlanes.empty() &&
        !copy.setDmaBufPlanes(frame.dmaBufPlanes))
      continue;
    copy.bufferLease = frame.bufferLease;
    copy.requestCpuFallback = frame.requestCpuFallback;
    bool ok = sink->tryEnqueue(std::move(copy));
    if (!ok) {
      // sink full or rejected; close dupfd by letting destructor close when leaving scope
    }
  }
}

} // namespace TrophyBackend
