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
// `CTRL_C_EVENT` in the parent while letting the child handle console signals,
// and exits with the child's exact exit code.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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

// Returns the full path of the current executable, growing the buffer as needed
// for paths longer than MAX_PATH.
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
      return buffer;
    }
    if (buffer.size() >= 32768) {
      return {};
    }
    buffer.resize(buffer.size() * 2);
  }
}

// Canonicalizes a path using GetFullPathNameW, resolving `..` segments.
auto GetFullPath(const std::wstring& path) -> std::wstring {
  DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return {};
  }
  std::wstring result(needed, L'\0');
  DWORD written =
      ::GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
  if (written == 0 || written >= needed) {
    return {};
  }
  result.resize(written);
  return result;
}

auto IsExistingFile(const std::wstring& path) -> bool {
  DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Locates `carbon-busybox.exe` relative to `self_path` across both packaged
// install trees (`bin/carbon.exe` -> `../lib/carbon/carbon-busybox.exe`,
// `lib/carbon/llvm/bin/*.exe` -> `../../carbon-busybox.exe`) and Bazel
// execution/runfiles trees (`toolchain/install/llvm/bin/*.exe` ->
// `../../carbon-busybox.exe`).
auto FindBusyboxPath(const std::wstring& self_path) -> std::wstring {
  size_t last_sep = self_path.find_last_of(L"\\/");
  if (last_sep == std::wstring::npos) {
    return {};
  }
  std::wstring self_dir = self_path.substr(0, last_sep);
  std::wstring_view self_name =
      std::wstring_view(self_path).substr(last_sep + 1);

  bool is_carbon_driver = EqualsIgnoreCase(self_name, L"carbon.exe") ||
                          EqualsIgnoreCase(self_name, L"carbon");

  const std::wstring_view carbon_candidates[] = {
      L"..\\lib\\carbon\\carbon-busybox.exe",
      L"..\\..\\carbon-busybox.exe",
      L"carbon-busybox.exe",
  };
  const std::wstring_view tool_candidates[] = {
      L"..\\..\\carbon-busybox.exe",
      L"..\\lib\\carbon\\carbon-busybox.exe",
      L"carbon-busybox.exe",
  };

  const auto& candidates =
      is_carbon_driver ? carbon_candidates : tool_candidates;
  for (std::wstring_view rel : candidates) {
    std::wstring joined = self_dir;
    joined.push_back(L'\\');
    joined.append(rel);
    std::wstring full = GetFullPath(joined);
    if (!full.empty() && !EqualsIgnoreCase(full, self_path) &&
        IsExistingFile(full)) {
      return full;
    }
  }
  return {};
}

}  // namespace

auto main() -> int {
  std::wstring self_path = GetSelfPath();
  if (self_path.empty()) {
    PrintError("failed to determine launcher path", L"", ::GetLastError());
    return 1;
  }

  std::wstring busybox_path = FindBusyboxPath(self_path);
  if (busybox_path.empty()) {
    PrintError("unable to locate carbon-busybox.exe relative to", self_path, 0);
    return 1;
  }

  // Pass the launcher's full path to `carbon-busybox.exe` via the Win32 UTF-16
  // environment block. With `carbon_busybox.manifest.xml` setting
  // `activeCodePage=UTF-8`, UCRT exposes this variable as UTF-8 to
  // `std::getenv("CARBON_ARGV0_OVERRIDE")`.
  if (!::SetEnvironmentVariableW(L"CARBON_ARGV0_OVERRIDE", self_path.c_str())) {
    PrintError("failed to set CARBON_ARGV0_OVERRIDE for", self_path,
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

  // Start the child suspended so we can assign it to the Job Object before it
  // executes and ignore CTRL+C in the parent without the child inheriting the
  // ignored-CTRL+C flag.
  if (!::CreateProcessW(busybox_path.c_str(), cmd_line.data(),
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

  // Ignore CTRL_C_EVENT in the parent so the child handles console interrupts
  // directly while the launcher waits to forward its exit code.
  ::SetConsoleCtrlHandler(nullptr, TRUE);

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
