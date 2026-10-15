#ifndef IDYDB_TEST_PROCESS_H
#define IDYDB_TEST_PROCESS_H

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace idydb_test {
inline std::string hex(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
inline std::string unhex(const std::string& value) {
    if (value.size() % 2) throw std::runtime_error("invalid child path encoding");
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("invalid child path encoding");
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); i += 2)
        result += static_cast<char>((nibble(value[i]) << 4) | nibble(value[i + 1]));
    return result;
}
#if defined(_WIN32)
inline std::wstring wide(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (!count) throw std::runtime_error("invalid UTF-8 child argument");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), count))
        throw std::runtime_error("cannot convert child argument");
    result.pop_back();
    return result;
}
inline std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
#endif
inline std::filesystem::path executable(const char* argv0) {
#if defined(_WIN32)
    (void)argv0;
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) throw std::runtime_error("cannot find test executable");
        if (count < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), count));
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::filesystem::absolute(argv0);
#endif
}
inline int child(const std::filesystem::path& program, const std::vector<std::string>& arguments,
                 std::chrono::seconds timeout = std::chrono::seconds(30)) {
#if defined(_WIN32)
    std::wstring command = quote(program.native());
    for (const auto& argument : arguments) command += L" " + quote(wide(argument));
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot start test child: " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count() * 1000));
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        throw std::runtime_error("test child timed out or could not be waited for");
    }
    DWORD status = 0;
    const BOOL success = GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (!success) throw std::runtime_error("cannot obtain test child status");
    return static_cast<int>(status);
#else
    std::vector<std::string> owned = {program.string()};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : owned) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t process = ::fork();
    if (process < 0) throw std::runtime_error("cannot fork test child");
    if (!process) { ::execv(program.c_str(), argv.data()); ::_exit(127); }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(process, &status, WNOHANG);
        if (result == process) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (result < 0 && errno != EINTR) throw std::runtime_error("cannot wait for test child");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(process, SIGKILL);
            while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("test child timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}
[[noreturn]] inline void crash_exit(unsigned status) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), status);
#endif
    std::_Exit(static_cast<int>(status));
}
}
#endif
