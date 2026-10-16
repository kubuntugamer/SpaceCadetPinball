#ifndef UPDATE_H
#define UPDATE_H

#include <string>

// Lightweight self-update helper. Checks the project's GitHub releases for a
// newer version, downloads the platform-appropriate asset and hands it off to
// the OS. All network work happens on a background thread; the UI polls the
// status each frame.
namespace update {

enum class Status { Idle, Checking, Available, UpToDate, Error };

struct ReleaseInfo {
	std::string version;       // e.g. "2.2.3" (tag without prefix)
	std::string release_url;   // html_url of the release
	std::string download_url;  // browser_download_url of the platform asset
	std::string notes;         // release body (markdown)
};

Status GetStatus();
const ReleaseInfo& GetInfo();
const std::string& GetError();
float GetProgress();          // 0..1 while downloading

void CheckAsync();            // start a background check (no-op if busy)
void DownloadAsync();         // download the available asset (no-op if busy)
void OpenReleasePage();       // open release_url in the browser
void Reset();                 // back to Idle

// Draw the update dialog. Call every frame from the UI thread.
void Render();

} // namespace update

#endif
