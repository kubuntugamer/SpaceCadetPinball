#ifndef UPDATE_H
#define UPDATE_H

#include <string>

// Self-update helper. Checks the project's GitHub releases for a newer version.
// On Linux, when the running binary is owned by a package manager (dpkg/rpm),
// the update is performed through that package manager (pkexec apt/dnf) after
// downloading the .deb/.rpm from the release; otherwise the platform asset is
// downloaded and handed off to the OS.
namespace update {

enum class Status { Idle, Checking, Available, UpToDate, Error, Installing, Installed };

struct ReleaseInfo {
	std::string version;
	std::string release_url;
	std::string download_url;   // platform asset (portable flow)
	std::string package_url;    // .deb/.rpm asset when applicable
	std::string notes;
};

Status GetStatus();
const ReleaseInfo& GetInfo();
const std::string& GetError();
float GetProgress();

bool IsPackageManaged();     // true if dpkg/rpm owns the running binary

void CheckAsync();            // background check (no-op if busy)
void InstallAsync();          // download + install (package manager or portable)
void OpenReleasePage();

void Reset();

// Draw the update dialog. Call every frame from the UI thread.
void Render();

// Trigger a check on startup if the user has it enabled. Call once.
void CheckOnStartup();

} // namespace update

#endif
