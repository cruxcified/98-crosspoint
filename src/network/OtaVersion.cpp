#include "OtaVersion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ota_version {

namespace {
constexpr char kForkMarker[] = "-w98.";
}  // namespace

int forkBuild(const char* version) {
  const char* marker = strstr(version, kForkMarker);
  if (!marker) return 0;
  return atoi(marker + sizeof(kForkMarker) - 1);
}

bool isNewer(const char* latest, const char* current) {
  if (latest[0] == '\0' || strcmp(latest, current) == 0) return false;

  // Fork release: the build number alone decides. Any fork release replaces a
  // build without one, such as a local "2.0.0-x4pro" flashed from SD.
  const int latestBuild = forkBuild(latest);
  if (latestBuild > 0) return latestBuild > forkBuild(current);

  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;
  sscanf(latest, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(current, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  if (latestMajor != currentMajor) return latestMajor > currentMajor;
  if (latestMinor != currentMinor) return latestMinor > currentMinor;
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // Same release number: an RC build is older than the final release.
  return strstr(current, "-rc") != nullptr;
}

}  // namespace ota_version
