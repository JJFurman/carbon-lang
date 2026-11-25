// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Lightweight Win32 launcher stub for Carbon's busybox installation.
//
// Installed as `bin/carbon.exe` and `lib/carbon/llvm/bin/<tool>.exe` so
// Windows installations work without symlinks or Developer Mode. Sets
// `CARBON_ARGV0_OVERRIDE` to the launcher's own path, spawns the adjacent
// `carbon-busybox.exe` with the exact original command line, keeps the child
// bound to a Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), ignores
// `CTRL_C_EVENT` and `CTRL_BREAK_EVENT` in the parent while letting the child
// handle console signals, and exits with the child's exact exit code.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

// Converts a UTF-16 string to UTF-8 for diagnostics.
auto WideToUtf8(std::wstring_view wide) -> std::string {
  if (wide.empty()) {
    return {};
  }
  int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                       static_cast<int>(wide.size()), nullptr,
                                       0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string utf8(static_cast<size_t>(required), '\0');
  int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                      static_cast<int>(wide.size()),
                                      utf8.data(), required, nullptr, nullptr);
  if (written <= 0) {
    return {};
  }
  utf8.resize(static_cast<size_t>(written));
  return utf8;
}

auto PrintError(std::string_view message, std::wstring_view path,
                DWORD error_code) -> void {
  std::string utf8_path = WideToUtf8(path);
  if (error_code != 0) {
    std::fprintf(stderr, "carbon-launcher: %.*s '%s' (Win32 error %lu)\n",
                 static_cast<int>(message.size()), message.data(),
                 utf8_path.c_str(), static_cast<unsigned long>(error_code));
  } else {
    std::fprintf(stderr, "carbon-launcher: %.*s '%s'\n",
                 static_cast<int>(message.size()), message.data(),
                 utf8_path.c_str());
  }
}

auto EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) -> bool {
  return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()),
                                /*bIgnoreCase=*/TRUE) == CSTR_EQUAL;
}

// Strips a `\\?\` or `\\?\UNC\` Win32 extended-length prefix if present.
auto StripExtendedPathPrefix(std::wstring_view path) -> std::wstring {
  if (path.size() >= 8 &&
      EqualsIgnoreCase(path.substr(0, 8), L"\\\\?\\UNC\\")) {
    std::wstring result = L"\\\\";
    result.append(path.substr(8));
    return result;
  }
  if (path.size() >= 4 && path.substr(0, 4) == L"\\\\?\\") {
    return std::wstring(path.substr(4));
  }
  return std::wstring(path);
}

// Adds a `\\?\` or `\\?\UNC\` prefix to normalized absolute paths approaching
// `MAX_PATH` so Win32 APIs succeed even when HKLM `LongPathsEnabled` is unset.
auto ToWin32LongPath(const std::wstring& path) -> std::wstring {
  constexpr size_t LongPathThreshold = MAX_PATH - 12;
  if (path.size() < LongPathThreshold || path.starts_with(L"\\\\?\\")) {
    return path;
  }
  std::wstring normalized = path;
  for (wchar_t& ch : normalized) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  if (normalized.starts_with(L"\\\\")) {
    std::wstring result = L"\\\\?\\UNC\\";
    result.append(normalized.substr(2));
    return result;
  }
  std::wstring result = L"\\\\?\\";
  result.append(normalized);
  return result;
}

auto BaseName(std::wstring_view path) -> std::wstring_view {
  size_t last_sep = path.find_last_of(L"\\/");
  if (last_sep == std::wstring_view::npos) {
    return path;
  }
  return path.substr(last_sep + 1);
}

// Walks up `levels` parent directory components from a normalized path.
auto ParentDir(std::wstring_view dir, size_t levels) -> std::wstring_view {
  while (levels > 0) {
    size_t last_sep = dir.find_last_of(L"\\/");
    if (last_sep == std::wstring_view::npos) {
      return {};
    }
    dir = dir.substr(0, last_sep);
    --levels;
  }
  return dir;
}

// Returns the full path of the current executable, growing the buffer as needed
// for paths longer than `MAX_PATH`.
auto GetSelfPath() -> std::wstring {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(),
                                     static_cast<DWORD>(buffer.size()));
    if (len == 0) {
      return {};
    }
    if (len < buffer.size()) {
      buffer.resize(len);
      return StripExtendedPathPrefix(buffer);
    }
    if (buffer.size() >= 32768) {
      return {};
    }
    buffer.resize(buffer.size() * 2);
  }
}

auto IsExistingFile(const std::wstring& path) -> bool {
  std::wstring win_path = ToWin32LongPath(path);
  DWORD attrs = ::GetFileAttributesW(win_path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Resolves symlinks/reparse points to the final canonical path on disk.
auto ResolveFinalPath(const std::wstring& path) -> std::wstring {
  std::wstring win_path = ToWin32LongPath(path);
  HANDLE file =
      ::CreateFileW(win_path.c_str(), /*dwDesiredAccess=*/0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  DWORD needed =
      ::GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
  if (needed == 0) {
    ::CloseHandle(file);
    return {};
  }
  std::wstring buffer(needed, L'\0');
  DWORD written = ::GetFinalPathNameByHandleW(file, buffer.data(), needed,
                                              FILE_NAME_NORMALIZED);
  ::CloseHandle(file);
  if (written == 0 || written >= needed) {
    return {};
  }
  buffer.resize(written);
  return StripExtendedPathPrefix(buffer);
}

struct RelativeCandidate {
  size_t parents_up;
  std::wstring_view suffix;
};

// Locates `carbon-busybox.exe` relative to `base_path` across both packaged
// install trees (`bin/carbon.exe` -> `../lib/carbon/carbon-busybox.exe`,
// `lib/carbon/llvm/bin/*.exe` -> `../../carbon-busybox.exe`) and Bazel
// execution/runfiles trees (`toolchain/install/llvm/bin/*.exe` ->
// `../../carbon-busybox.exe`).
auto FindBusyboxRelativeTo(const std::wstring& base_path) -> std::wstring {
  size_t last_sep = base_path.find_last_of(L"\\/");
  if (last_sep == std::wstring::npos) {
    return {};
  }
  std::wstring_view base_dir = std::wstring_view(base_path).substr(0, last_sep);
  std::wstring_view base_name =
      std::wstring_view(base_path).substr(last_sep + 1);

  bool is_carbon_driver = EqualsIgnoreCase(base_name, L"carbon.exe") ||
                          EqualsIgnoreCase(base_name, L"carbon");

  constexpr std::array<RelativeCandidate, 3> CarbonCandidates = {{
      {.parents_up = 1, .suffix = L"lib\\carbon\\carbon-busybox.exe"},
      {.parents_up = 2, .suffix = L"carbon-busybox.exe"},
      {.parents_up = 0, .suffix = L"carbon-busybox.exe"},
  }};
  constexpr std::array<RelativeCandidate, 3> ToolCandidates = {{
      {.parents_up = 2, .suffix = L"carbon-busybox.exe"},
      {.parents_up = 1, .suffix = L"lib\\carbon\\carbon-busybox.exe"},
      {.parents_up = 0, .suffix = L"carbon-busybox.exe"},
  }};

  const auto& candidates = is_carbon_driver ? CarbonCandidates : ToolCandidates;
  for (const RelativeCandidate& candidate : candidates) {
    std::wstring_view ancestor = ParentDir(base_dir, candidate.parents_up);
    if (ancestor.empty()) {
      continue;
    }
    std::wstring full(ancestor);
    full.push_back(L'\\');
    full.append(candidate.suffix);
    if (!EqualsIgnoreCase(full, base_path) && IsExistingFile(full)) {
      return full;
    }
  }
  return {};
}

// Handles console Ctrl+C and Ctrl+Break events in the launcher so
// `carbon-busybox.exe` can run its own signal cleanup and exit cleanly instead
// of being hard-killed by the launcher's Job Object closing early.
auto WINAPI ConsoleCtrlHandler(DWORD ctrl_type) -> BOOL {
  return (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) ? TRUE
                                                                      : FALSE;
}

}  // namespace

auto main() -> int {
  std::wstring self_path = GetSelfPath();
  if (self_path.empty()) {
    PrintError("failed to determine launcher path", L"", ::GetLastError());
    return 1;
  }

  std::wstring argv0_override = self_path;
  std::wstring busybox_path = FindBusyboxRelativeTo(self_path);
  if (busybox_path.empty()) {
    // Handle symlinked shims (e.g. package-manager links pointing into an
    // install tree) by resolving the final on-disk target path.
    std::wstring final_path = ResolveFinalPath(self_path);
    if (!final_path.empty() && !EqualsIgnoreCase(final_path, self_path)) {
      busybox_path = FindBusyboxRelativeTo(final_path);
      if (!busybox_path.empty() &&
          EqualsIgnoreCase(BaseName(final_path), BaseName(self_path))) {
        argv0_override = final_path;
      }
    }
  }
  if (busybox_path.empty()) {
    PrintError("unable to locate carbon-busybox.exe relative to", self_path, 0);
    return 1;
  }

  // Pass the launcher's full path to `carbon-busybox.exe` via the Win32 UTF-16
  // environment block. With `carbon_busybox.manifest.xml` setting
  // `activeCodePage=UTF-8`, UCRT exposes this variable as UTF-8 to
  // `std::getenv("CARBON_ARGV0_OVERRIDE")`.
  if (!::SetEnvironmentVariableW(L"CARBON_ARGV0_OVERRIDE",
                                 argv0_override.c_str())) {
    PrintError("failed to set CARBON_ARGV0_OVERRIDE for", argv0_override,
               ::GetLastError());
    return 1;
  }

  // Bind the child process tree to a Job Object so terminating the launcher
  // does not leave orphaned compiler or linker processes behind.
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
    job_info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &job_info, sizeof(job_info))) {
      ::CloseHandle(job);
      job = nullptr;
    }
  }

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};

  const wchar_t* raw_cmd_line = ::GetCommandLineW();
  std::wstring cmd_line = raw_cmd_line != nullptr ? raw_cmd_line : L"";
  std::wstring launch_path = ToWin32LongPath(busybox_path);

  // Start the child suspended so we can assign it to the Job Object before it
  // executes and install the parent's console ctrl handler before resuming.
  if (!::CreateProcessW(launch_path.c_str(), cmd_line.data(),
                        /*lpProcessAttributes=*/nullptr,
                        /*lpThreadAttributes=*/nullptr,
                        /*bInheritHandles=*/TRUE, CREATE_SUSPENDED,
                        /*lpEnvironment=*/nullptr,
                        /*lpCurrentDirectory=*/nullptr, &startup_info,
                        &process_info)) {
    DWORD err = ::GetLastError();
    PrintError("failed to launch", busybox_path, err);
    if (job != nullptr) {
      ::CloseHandle(job);
    }
    return 1;
  }

  if (job != nullptr) {
    if (!::AssignProcessToJobObject(job, process_info.hProcess)) {
      ::CloseHandle(job);
      job = nullptr;
    }
  }

  // Ignore CTRL_C_EVENT and CTRL_BREAK_EVENT in the parent so the child
  // handles console interrupts cleanly while the launcher waits to forward its
  // exit code.
  ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  if (::ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
    DWORD err = ::GetLastError();
    PrintError("failed to resume", busybox_path, err);
    ::TerminateProcess(process_info.hProcess, 1);
    ::CloseHandle(process_info.hThread);
    ::CloseHandle(process_info.hProcess);
    if (job != nullptr) {
      ::CloseHandle(job);
    }
    return 1;
  }
  ::CloseHandle(process_info.hThread);

  if (::WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0) {
    DWORD err = ::GetLastError();
    PrintError("failed while waiting for", busybox_path, err);
    ::CloseHandle(process_info.hProcess);
    if (job != nullptr) {
      ::CloseHandle(job);
    }
    return 1;
  }

  DWORD exit_code = 1;
  if (!::GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    DWORD err = ::GetLastError();
    PrintError("failed to get exit code for", busybox_path, err);
    exit_code = 1;
  }

  ::CloseHandle(process_info.hProcess);
  if (job != nullptr) {
    ::CloseHandle(job);
  }
  return static_cast<int>(exit_code);
}

#else

auto main() -> int { return 1; }

#endif  // _WIN32
