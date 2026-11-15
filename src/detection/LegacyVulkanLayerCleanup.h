/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend::LegacyVulkanLayerCleanup {

/** Path of the implicit-layer manifest the retired Vulkan layer wrote. */
QString manifestPath();

/**
 * @brief Remove the retired Vulkan layer's registration and durable copies.
 *
 * Earlier builds could enroll an implicit Vulkan layer (its manifest under
 * the user's data dir) that reported START/STOP to the detection socket. The
 * layer is gone now, so a stale manifest would make every future Vulkan
 * process try to load a missing library. Remove only Trophy's own manifest
 * (identified by the layer name / library name), keep foreign manifests and
 * unrelated files untouched, and delete the durable reporter copies the
 * enrollment created. A missing manifest is a no-op.
 *
 * @return false only when something owned could not be removed.
 */
bool removeOwnedRegistration();

} // namespace TrophyBackend::LegacyVulkanLayerCleanup
