#include "pch.h"
#include "update.h"
#include "winmain.h"
#include "options.h"

#include <mutex>
#include <thread>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>       // MSVC's _unlink lives here, not in <unistd.h>
#define POPEN _popen
#define PCLOSE _pclose
#define SCP_UNLINK _unlink
#else
#include <unistd.h>   // readlink() in exe_path(), unlink() in SCP_UNLINK
#define POPEN popen
#define PCLOSE pclose
#define SCP_UNLINK unlink
#endif

namespace {
std::mutex g_mutex;
update::Status g_status = update::Status::Idle;
update::ReleaseInfo g_info;
std::string g_error;
float g_progress = 0.0f;
bool g_downloading = false;
bool g_checking = false;
bool g_installing = false;
bool g_installed = false;

std::string run_command(const std::string& cmd) {
	std::string out;
	FILE* f = POPEN(cmd.c_str(), "r");
	if (!f) return out;
	char buf[4096];
	while (fgets(buf, sizeof(buf), f)) out += buf;
	PCLOSE(f);
	return out;
}

std::string extract_string(const std::string& json, const std::string& key, size_t start = 0) {
	std::string needle = "\"" + key + "\"";
	size_t pos = json.find(needle, start);
	if (pos == std::string::npos) return "";
	pos = json.find(':', pos);
	if (pos == std::string::npos) return "";
	pos = json.find('"', pos);
	if (pos == std::string::npos) return "";
	size_t end = json.find('"', pos + 1);
	if (end == std::string::npos) return "";
	return json.substr(pos + 1, end - pos - 1);
}

std::vector<std::string> extract_all(const std::string& json, const std::string& key) {
	std::vector<std::string> res;
	std::string needle = "\"" + key + "\"";
	size_t pos = 0;
	while ((pos = json.find(needle, pos)) != std::string::npos) {
		size_t c = json.find(':', pos);
		if (c == std::string::npos) break;
		size_t q = json.find('"', c);
		if (q == std::string::npos) break;
		size_t e = json.find('"', q + 1);
		if (e == std::string::npos) break;
		res.push_back(json.substr(q + 1, e - q - 1));
		pos = e + 1;
	}
	return res;
}

// Strip a leading prefix like "Release_" or "v" from a tag.
std::string strip_prefix(const std::string& tag) {
	std::string v = tag;
	// remove everything up to and including the last underscore or 'v'/'V'
	size_t us = v.rfind('_');
	if (us != std::string::npos && us + 1 < v.size()) v = v.substr(us + 1);
	if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
	return v;
}

struct Version { int major = 0, minor = 0, patch = 0; };
Version parse_version(const std::string& v) {
	Version out;
	int i = 0;
	auto read = [&](int& field) {
		size_t dot = v.find('.', i);
		std::string part = (dot == std::string::npos) ? v.substr(i) : v.substr(i, dot - i);
		try { field = std::stoi(part); } catch (...) { field = 0; }
		i = (dot == std::string::npos) ? (int)v.size() : (int)dot + 1;
	};
	read(out.major); read(out.minor); read(out.patch);
	return out;
}

bool is_newer(const std::string& remote, const std::string& current) {
	Version r = parse_version(remote);
	Version c = parse_version(current);
	if (r.major != c.major) return r.major > c.major;
	if (r.minor != c.minor) return r.minor > c.minor;
	return r.patch > c.patch;
}

bool is_platform_asset(const std::string& url) {
#ifdef _WIN32
#ifdef _WIN64
	return url.find("x64Win.zip") != std::string::npos;
#else
	return url.find("x86Win.zip") != std::string::npos;
#endif
#elif defined(__APPLE__)
	return url.find(".dmg") != std::string::npos;
#else
	// Prefer AppImage (self-contained, runnable) over tarball.
	if (url.find(".AppImage") != std::string::npos) return true;
	return false;
#endif
}

std::string pick_asset(const std::vector<std::string>& urls) {
	// First try platform-specific asset.
	for (auto& u : urls) if (is_platform_asset(u)) return u;
	// Fallback: first url.
	if (!urls.empty()) return urls.front();
	return "";
}

std::string temp_dir() {
#ifdef _WIN32
	char buf[MAX_PATH];
	GetTempPathA(MAX_PATH, buf);
	return std::string(buf);
#else
	const char* t = getenv("TMPDIR");
	if (t && *t) return std::string(t) + "/";
	return "/tmp/";
#endif
}

bool ends_with(const std::string& s, const std::string& suffix) {
	return s.size() >= suffix.size() &&
	       s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Single-quote a value so a shell takes it verbatim, whatever it contains.
std::string shell_quote(const std::string& s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	return out + "'";
}

// Absolute path of the running executable, or "" when it cannot be resolved.
std::string exe_path() {
#ifdef _WIN32
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	return n ? std::string(buf, n) : std::string();
#else
	char buf[4096];
	auto len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len <= 0) return std::string();
	buf[len] = '\0';
	return std::string(buf);
#endif
}

// Which package manager owns this install, if any.
enum class PkgKind { None, Deb, Rpm };

// Resolved once per process: it forks two commands and the answer cannot
// change while the game is running. /proc/self/exe does not exist on macOS, so
// exe_path() comes back empty there and the answer is None - correct, since
// macOS installs arrive as a .dmg and update through the portable path.
PkgKind pkg_kind() {
#ifdef _WIN32
	return PkgKind::None;
#else
	static const PkgKind kind = [] {
		std::string exe = exe_path();
		if (exe.empty()) return PkgKind::None;
		if (system(("dpkg -S " + shell_quote(exe) + " >/dev/null 2>&1").c_str()) == 0)
			return PkgKind::Deb;
		if (system(("rpm -qf " + shell_quote(exe) + " >/dev/null 2>&1").c_str()) == 0)
			return PkgKind::Rpm;
		return PkgKind::None;
	}();
	return kind;
#endif
}

// First release asset with the given extension, or "".
std::string find_asset(const std::vector<std::string>& urls, const std::string& ext) {
	for (const auto& u : urls)
		if (ends_with(u, ext)) return u;
	return "";
}

// Is `name` runnable? access() resolves relative to the working directory and
// never searches PATH, so ask the shell instead.
bool have_command(const char* name) {
#ifdef _WIN32
	(void)name;
	return false;   // no polkit/apt/dnf on Windows
#else
	return system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

void set_error(const std::string& msg) {
	std::lock_guard<std::mutex> lk(g_mutex);
	g_status = update::Status::Error;
	g_error = msg;
	g_installing = false;
}

// Runs the package manager over `file`, returns 0 on success.
//
// Exactly one attempt is made to gain administrator rights: offering a second
// polkit prompt after the user dismissed the first would be hostile. If that
// fails, or no helper exists, the plain install runs - it succeeds when the
// game already has the privileges, otherwise it fails fast without prompting.
int install_package(PkgKind kind, const std::string& file) {
	const std::string f = shell_quote(file);
	const bool rpm = kind == PkgKind::Rpm;
	const std::string manager = rpm ? "dnf" : "apt-get";
	const std::string plain = rpm ? ("rpm -i " + f) : ("dpkg -i " + f);

	std::string admin;
	if (have_command("pkexec")) admin = "pkexec " + manager + " install -y " + f;
	else if (have_command("sudo")) admin = "sudo " + manager + " install -y " + f;

	if (!admin.empty() && system(admin.c_str()) == 0) return 0;
	return system(plain.c_str()) == 0 ? 0 : 1;
}

void do_check() {
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_status = update::Status::Checking;
		g_error.clear();
	}
	std::string url = "https://api.github.com/repos/kubuntugamer/SpaceCadetPinball/releases/latest";
	std::string cmd = "curl -s -L -m 20 -A \"SpaceCadetPinball/" + std::string(winmain::Version) + "\" \"" + url + "\"";
	std::string json = run_command(cmd);

	if (json.empty()) {
		std::lock_guard<std::mutex> lk(g_mutex);
		g_status = update::Status::Error;
		g_error = "Could not reach GitHub (curl missing or network error).";
		g_checking = false;
		return;
	}
	if (json.find("\"message\"") != std::string::npos && json.find("Not Found") != std::string::npos) {
		// possibly rate limited or not found
	}

	std::string tag = extract_string(json, "tag_name");
	std::string html = extract_string(json, "html_url");
	std::string body = extract_string(json, "body");
	auto urls = extract_all(json, "browser_download_url");
	std::string asset = pick_asset(urls);

	// A package-managed binary updates through dpkg/dnf rather than the
	// portable asset above, so it needs the matching package from this release.
	std::string pkg;
	switch (pkg_kind()) {
	case PkgKind::Deb: pkg = find_asset(urls, ".deb"); break;
	case PkgKind::Rpm: pkg = find_asset(urls, ".rpm"); break;
	default: break;
	}

	std::string remote_ver = strip_prefix(tag);

	update::ReleaseInfo info;
	info.version = remote_ver;
	info.release_url = html.empty() ? ("https://github.com/kubuntugamer/SpaceCadetPinball/releases/tag/" + tag) : html;
	info.download_url = asset;
	info.package_url = pkg;
	info.notes = body;

	bool newer = !remote_ver.empty() && is_newer(remote_ver, winmain::Version);

	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_info = info;
		if (remote_ver.empty()) {
			g_status = update::Status::Error;
			g_error = "Could not parse release info.";
		} else if (newer) {
			g_status = update::Status::Available;
		} else {
			g_status = update::Status::UpToDate;
		}
		g_checking = false;
	}
}

void do_download() {
	std::string url;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		url = g_info.download_url;
		g_downloading = true;
		g_progress = 0.0f;
	}
	if (url.empty()) {
		std::lock_guard<std::mutex> lk(g_mutex);
		g_downloading = false;
		g_error = "No download URL for this platform.";
		g_status = update::Status::Error;
		return;
	}
	// derive filename
	std::string name = url.substr(url.find_last_of('/') + 1);
	std::string dest = temp_dir() + name;

	std::string cmd = "curl -s -L -m 300 -o \"" + dest + "\" \"" + url + "\"";
	run_command(cmd);

	// verify file exists
	FILE* f = fopen(dest.c_str(), "rb");
	bool ok = f != nullptr;
	if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f); if (sz <= 0) ok = false; }

	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_downloading = false;
		g_progress = ok ? 1.0f : 0.0f;
		if (!ok) {
			g_status = update::Status::Error;
			g_error = "Download failed.";
			return;
		}
	}

	// Hand off to the OS.
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
	std::string cmd2 = "open \"" + dest + "\"";
	system(cmd2.c_str());
#else
	// Make executable if it's an AppImage, then launch detached.
	if (dest.find(".AppImage") != std::string::npos) {
		chmod(dest.c_str(), 0755);
		std::string cmd2 = "chmod +x \"" + dest + "\" && (\"" + dest + "\" &) >/dev/null 2>&1";
		system(cmd2.c_str());
	} else {
		std::string cmd2 = "xdg-open \"" + dest + "\" >/dev/null 2>&1 &";
		system(cmd2.c_str());
	}
#endif
}
} // namespace

namespace update {

Status GetStatus() {
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_status;
}
const ReleaseInfo& GetInfo() {
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_info;
}
const std::string& GetError() {
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_error;
}
float GetProgress() {
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_progress;
}

void CheckAsync() {
	bool expected = false;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (g_checking || g_status == Status::Checking) return;
		g_checking = true;
	}
	std::thread([]() { do_check(); }).detach();
}

void CheckOnStartup() {
	// Once per process. WinMain runs inside a do/while(restart) loop, so
	// without this an update that was just installed would be re-checked for
	// the moment the game relaunches into it.
	static bool checked = false;
	if (checked) return;
	checked = true;

	// Option defaults to on; see Options -> Check for Updates on Startup.
	// Read through options::Options, the public accessor that winmain::Options
	// aliases - winmain's own member is private to that class.
	if (!options::Options.CheckForUpdatesOnStartup.V) return;
	CheckAsync();
}


bool IsPackageManaged() {
	return pkg_kind() != PkgKind::None;
}

void DownloadAsync() {
	bool expected = false;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (g_downloading) return;
	}
	std::thread([]() { do_download(); }).detach();
}

void OpenReleasePage() {
	std::string url;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		url = g_info.release_url;
	}
	if (url.empty()) url = "https://github.com/kubuntugamer/SpaceCadetPinball/releases";
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
	std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
	system(cmd.c_str());
#else
	std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
	system(cmd.c_str());
#endif
}

void Reset() {
	std::lock_guard<std::mutex> lk(g_mutex);
	g_status = Status::Idle;
	g_error.clear();
	g_info = ReleaseInfo{};
	g_progress = 0.0f;
}

void Render() {
	// Trigger dialog when an update is found.
	static bool popup_open = false;
	Status st = GetStatus();
	if (st == Status::Available && !popup_open) {
		ImGui::OpenPopup("Update Available");
		popup_open = true;
	}
	if (st == Status::UpToDate) {
		// transient: show nothing, but allow manual re-check
	}
	if (st == Status::Error) {
		// show error popup once
	}

	if (ImGui::BeginPopupModal("Update Available", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		const auto& info = GetInfo();

		if (st == Status::Installed) {
			ImGui::Text("Update %s installed.", info.version.c_str());
			ImGui::TextWrapped("Restart the game to finish applying it.");
			ImGui::Separator();
			if (ImGui::Button("Restart Now")) {
				ImGui::CloseCurrentPopup();
				popup_open = false;
				Reset();
				winmain::Restart();
			}
			ImGui::SameLine();
			if (ImGui::Button("Later")) {
				Reset();
				ImGui::CloseCurrentPopup();
				popup_open = false;
			}
		} else if (st == Status::Installing || g_downloading) {
			// Shown for the whole download. The work runs on a background
			// thread and never holds g_mutex while blocked, so the game keeps
			// rendering - including while polkit waits for a password.
			ImGui::Text("Updating to %s...", info.version.c_str());
			ImGui::ProgressBar(GetProgress(), ImVec2(320.0f, 0.0f));
			ImGui::TextUnformatted(GetProgress() < 0.5f ? "Downloading package..."
			                                            : "Installing package...");
		} else {
			ImGui::Text("A new version is available: %s", info.version.c_str());
			ImGui::Text("Current version: %s", winmain::Version);
			ImGui::Separator();
			if (!info.notes.empty()) {
				ImGui::TextWrapped("%s", info.notes.c_str());
				ImGui::Separator();
			}
			if (!info.package_url.empty()) {
				// Package-managed install: dpkg/dnf with administrator rights.
				// The portable asset stays reachable alongside it.
				if (ImGui::Button("Install Update")) InstallAsync();
				ImGui::SameLine();
			}
			if (ImGui::Button("Download Update")) DownloadAsync();
			ImGui::SameLine();
			if (ImGui::Button("Open Release Page")) OpenReleasePage();
			ImGui::SameLine();
			if (ImGui::Button("Later")) {
				Reset();
				ImGui::CloseCurrentPopup();
				popup_open = false;
			}
		}
		ImGui::EndPopup();
	}

	// Error popup
	static bool err_open = false;
	if (st == Status::Error && !err_open) {
		ImGui::OpenPopup("Update Check Failed");
		err_open = true;
	}
	if (ImGui::BeginPopupModal("Update Check Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", GetError().c_str());
		if (ImGui::Button("OK")) {
			Reset();
			ImGui::CloseCurrentPopup();
			err_open = false;
		}
		ImGui::EndPopup();
	}
	if (st != Status::Error) err_open = false;
	if (st == Status::Idle) popup_open = false;
}


void do_install() {
	// Nothing below may run under g_mutex: the download can take minutes and the
	// package manager blocks on the polkit prompt, which the user may take even
	// longer over. Render() calls GetStatus()/GetProgress() every frame, so
	// holding the lock across either would freeze the entire game.
	update::ReleaseInfo info;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		info = g_info;
		g_status = update::Status::Installing;
		g_installing = true;
		g_progress = 0.0f;
		g_installed = false;
		g_error.clear();
	}

	const PkgKind kind = pkg_kind();
	const std::string ext = kind == PkgKind::Rpm ? ".rpm" : ".deb";

	// Prefer the package this install owns; fall back to the portable asset so
	// the message can tell "no package for us" from "nothing at all".
	const std::string url = info.package_url.empty() ? info.download_url : info.package_url;
	if (url.empty()) {
		set_error("No update information available. Check for updates first.");
		return;
	}
	if (!ends_with(url, ext)) {
		set_error("This release has no " + ext + " package to install.");
		return;
	}

	const std::string name = url.substr(url.find_last_of('/') + 1);
	const std::string dest = temp_dir() + name;
	run_command("curl -s -L -m 300 -o " + shell_quote(dest) + " " + shell_quote(url));

	bool ok = false;
	if (FILE* f = fopen(dest.c_str(), "rb")) {
		fseek(f, 0, SEEK_END);
		ok = ftell(f) > 0;
		fclose(f);
	}
	if (!ok) {
		SCP_UNLINK(dest.c_str());
		set_error("Failed to download " + name + ".");
		return;
	}
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_progress = 0.5f;
	}

	const int rc = install_package(kind, dest);
	SCP_UNLINK(dest.c_str());

	if (rc != 0) {
		set_error("Could not install " + name + ". Get it from the release page instead.");
		return;
	}

	std::lock_guard<std::mutex> lk(g_mutex);
	g_installed = true;
	g_progress = 1.0f;
	g_status = update::Status::Installed;
	g_installing = false;
	g_error.clear();
}

void InstallAsync() {
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (g_installing || g_downloading || g_checking) return;
	}
	std::thread([]() { do_install(); }).detach();
}
} // namespace update
