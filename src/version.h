// Single place for the app version.
//   - compiled into the exe (shown in Settings > About, used by the update checker)
//   - written into the exe's version resource (src/app.rc), AudioEnhancer.iss reads it from there
// To release a new version: change the numbers below, run `make`, compile the installer,
// then upload release\AudioEnhancer-Setup.exe to a GitHub release whose tag is the same version (e.g. 1.0.1).
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_VERSION_STR "1.0.3"