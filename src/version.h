// Single source of the add-on version. Bump it with every delivered build and add a line to CHANGELOG.md.
// Used by the code (overlay, on-screen readout, ReShade.log) and by version.rc (file properties, ReShade's add-on list).
// Plain string literals on purpose: the resource compiler (rc.exe) does not join adjacent strings like C++ does.
#pragma once

#define ADDON_VERSION_MAJOR 1
#define ADDON_VERSION_MINOR 1
#define ADDON_VERSION_PATCH 0
#define ADDON_VERSION_STRING "1.1.0"
#define ADDON_VERSION_STAGE "release"
#define ADDON_VERSION_FULL "1.1.0 (release)"
// Shown in the on-screen readout: the version plus the stage while it is not a release ("" for releases).
#define ADDON_VERSION_SHOWN "1.1.0"
