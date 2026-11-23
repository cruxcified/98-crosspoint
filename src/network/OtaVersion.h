#pragma once

// Version ordering for OTA updates. Fork releases carry a "-w98.<build>" suffix
// (e.g. "2.0.0-w98.12") where <build> is the release workflow run number. It
// only grows, so it orders fork releases even when the upstream base changes.
namespace ota_version {

// Fork build number from a "-w98.<n>" suffix, or 0 when there is none.
int forkBuild(const char* version);

// True when `latest` should replace `current`.
bool isNewer(const char* latest, const char* current);

}  // namespace ota_version
