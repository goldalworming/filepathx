#ifndef VERSION_H
#define VERSION_H

/* Bump this together with the git tag when cutting a release (see
   docs/RELEASES.md). The auto-updater compares it against the latest
   GitHub release tag. */
#define APP_VERSION "0.7.11"

/* Same version in numeric form, for the VERSIONINFO block in resource.rc.
   Keep the three numbers in sync with APP_VERSION above. */
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 7
#define APP_VERSION_PATCH 11

#endif
