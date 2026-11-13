// Copyright 2026 GCSA
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "base/compiler_specific.h"

int wmain(int argc, wchar_t* argv[]) {
  // The CRT provides argc pointers. Each argument access is count-checked.
  if (argc == 2 &&
      UNSAFE_BUFFERS(std::wstring_view(argv[1])) == L"--access-fixture-grandchild") {
    ::Sleep(INFINITE);
    return 0;
  }
  std::error_code error;
  const auto directory = std::filesystem::current_path(error);
  if (error) {
    return 1;
  }
  const auto config_path = directory / L"config.json";
  if (argc != 4 || UNSAFE_BUFFERS(std::wstring_view(argv[1])) != L"run" ||
      UNSAFE_BUFFERS(std::wstring_view(argv[2])) != L"-config" ||
      UNSAFE_BUFFERS(std::filesystem::path(argv[3])) != config_path) {
    return 2;
  }
  {
    std::ifstream config(config_path);
    if (config.peek() != '{') {
      return 3;
    }
  }
  std::wstring child_command = L"\"access fixture\" --access-fixture-grandchild";
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION child = {};
  if (!::CreateProcessW(argv[0], child_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
    return 4;
  }
  ::CloseHandle(child.hProcess);
  ::CloseHandle(child.hThread);
  {
    std::ofstream pids(directory / L"fixture-pids");
    pids << ::GetCurrentProcessId() << ' ' << child.dwProcessId << '\n';
    pids.close();
    if (!pids) {
      return 5;
    }
  }
  ::Sleep(INFINITE);
  return 0;
}
