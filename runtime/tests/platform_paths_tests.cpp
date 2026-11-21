#include "platform/host_platform.h"

#include <iostream>
#include <cstdlib>

int main() {
    if (!RuntimePlatform::ExecutableDirectory()) {
        std::cerr << "unable to resolve the current executable directory\n";
        return 1;
    }

    const auto userData = RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest");
    if (userData.filename() != "WiiCompiledPlatformPathsTest") {
        std::cerr << "application-data directory lost its application name: " << userData << '\n';
        return 1;
    }
    if (RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != userData / "Logs") {
        std::cerr << "log directory is not derived from application data\n";
        return 1;
    }
#if defined(__APPLE__)
    if (userData.parent_path().filename() != "Application Support" ||
        userData.parent_path().parent_path().filename() != "Library") {
        std::cerr << "macOS application-data directory is not under ~/Library/Application Support: "
                  << userData << '\n';
        return 1;
    }
#endif

#if defined(__linux__)
    // These environment changes are confined to this test process.
    if (setenv("HOME", "/tmp/sin-test-home", 1) != 0) return 1;
    for (const char* xdg : {"/tmp/sin-test-data", "relative-data", ""}) {
        if (setenv("XDG_DATA_HOME", xdg, 1) != 0) return 1;
        const auto expected = std::filesystem::path(*xdg == '/' ?
            "/tmp/sin-test-data" : "/tmp/sin-test-home/.local/share") / "WiiCompiledPlatformPathsTest";
        if (RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest") != expected ||
            RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != expected / "Logs") {
            std::cerr << "Linux data directory did not honor absolute XDG/HOME paths\n";
            return 1;
        }
    }
    unsetenv("XDG_DATA_HOME");
    if (RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest") !=
        "/tmp/sin-test-home/.local/share/WiiCompiledPlatformPathsTest") return 1;
#endif
    return 0;
}
