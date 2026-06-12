#define _CRT_SECURE_NO_WARNINGS

#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>
#include "../../src/nvgt_plugin.h"

// Convert text from the Windows OEM codepage, used by many console programs,
// into UTF-8 for NVGT strings.
static std::string oem_to_utf8(const std::string& oem) {
    if (oem.empty()) return "";

    int wlen = MultiByteToWideChar(CP_OEMCP, 0, oem.c_str(), (int)oem.size(), NULL, 0);
    if (wlen <= 0) return oem;

    std::vector<wchar_t> wbuf(wlen);
    MultiByteToWideChar(CP_OEMCP, 0, oem.c_str(), (int)oem.size(), wbuf.data(), wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, NULL, 0, NULL, NULL);
    if (ulen <= 0) return oem;

    std::string utf8(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen, &utf8[0], ulen, NULL, NULL);
    return utf8;
}

static std::string get_temp_path_impl() {
    char buf[MAX_PATH];

    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len == 0 || len >= MAX_PATH) {
        return "C:\\Temp\\";
    }

    return std::string(buf);
}

// Make a unique log path without creating the file ahead of time.
static std::string make_temp_log_path() {
    std::ostringstream ss;
    ss << get_temp_path_impl()
       << "nvgt_cmd_"
       << (unsigned long)GetCurrentProcessId()
       << "_"
       << (unsigned long long)GetTickCount64()
       << "_"
       << (unsigned long)GetCurrentThreadId()
       << ".log";

    return ss.str();
}

static std::string read_file_contents(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return "";

    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// This is only for debug display now.
// The plugin no longer uses > redirection for sync capture.
static std::string build_process_command(const std::string& user_cmd, bool use_powershell) {
    if (use_powershell) {
        return "powershell.exe -NoProfile -NonInteractive -Command " + user_cmd;
    }

    return "cmd.exe /C " + user_cmd;
}

static HANDLE open_log_for_child_write(const std::string& log_path, std::string& error) {
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE log = CreateFileA(
        log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (log == INVALID_HANDLE_VALUE) {
        std::ostringstream ss;
        ss << "CreateFileA failed.\r\n"
           << "Windows error: " << GetLastError() << "\r\n"
           << "Log path:\r\n"
           << log_path << "\r\n";
        error = ss.str();
        return INVALID_HANDLE_VALUE;
    }

    // Make sure the child can inherit this handle.
    if (!SetHandleInformation(log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        std::ostringstream ss;
        ss << "SetHandleInformation failed.\r\n"
           << "Windows error: " << GetLastError() << "\r\n"
           << "Log path:\r\n"
           << log_path << "\r\n";
        error = ss.str();
        CloseHandle(log);
        return INVALID_HANDLE_VALUE;
    }

    return log;
}

static bool launch_process_to_log(
    const std::string& full_cmd,
    const std::string& log_path,
    bool wait_for_exit,
    DWORD* exit_code_out,
    std::string& error
) {
    HANDLE log = open_log_for_child_write(log_path, error);
    if (log == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;

    // Some programs dislike a null stdin. NUL is safer for non-interactive tools.
    HANDLE nul_in = CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (nul_in != INVALID_HANDLE_VALUE) {
        SetHandleInformation(nul_in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.hStdInput = nul_in;
    } else {
        si.hStdInput = NULL;
    }

    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmd_buf(full_cmd.begin(), full_cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        NULL,
        cmd_buf.data(),
        NULL,
        NULL,
        TRUE,              // inherit handles so stdout/stderr can go to log
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (nul_in != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_in);
    }

    if (!ok) {
        DWORD err = GetLastError();

        std::ostringstream ss;
        ss << "CreateProcessA failed.\r\n"
           << "Windows error: " << err << "\r\n"
           << "Command sent to CreateProcess:\r\n"
           << full_cmd << "\r\n"
           << "Log path:\r\n"
           << log_path << "\r\n";

        error = ss.str();
        CloseHandle(log);
        return false;
    }

    if (wait_for_exit) {
        WaitForSingleObject(pi.hProcess, INFINITE);

        if (exit_code_out != NULL) {
            DWORD code = 0;
            if (GetExitCodeProcess(pi.hProcess, &code)) {
                *exit_code_out = code;
            }
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Important:
    // For async, the child inherited its own copy of the log handle.
    // Closing our copy is fine; the child can still write.
    CloseHandle(log);

    return true;
}

static std::string cmd_run_impl(const std::string& command, bool use_powershell) {
    std::string log_path = make_temp_log_path();
    std::string full_cmd = build_process_command(command, use_powershell);

    DWORD exit_code = 0;
    std::string error;

    bool ok = launch_process_to_log(full_cmd, log_path, true, &exit_code, error);
    if (!ok) {
        return error;
    }

    // Give Windows/AV/indexers a tiny moment to release/flush the file.
    Sleep(50);

    std::string raw = read_file_contents(log_path);
    std::remove(log_path.c_str());

    if (raw.empty()) {
        std::ostringstream ss;
        ss << "Command ran but log was empty.\r\n"
           << "Exit code: " << exit_code << "\r\n"
           << "User command:\r\n"
           << command << "\r\n"
           << "Command sent to CreateProcess:\r\n"
           << full_cmd << "\r\n"
           << "Log path:\r\n"
           << log_path << "\r\n";

        return ss.str();
    }

    return use_powershell ? raw : oem_to_utf8(raw);
}

static int cmd_run_async_impl(const std::string& command, const std::string& log_path, bool use_powershell) {
    std::string full_cmd = build_process_command(command, use_powershell);

    DWORD exit_code = 0;
    std::string error;

    bool ok = launch_process_to_log(full_cmd, log_path, false, &exit_code, error);
    if (!ok) {
        // Preserve the old API: return nonzero on failure.
        // Also try to write the error into the requested log so the script can read it.
        std::ofstream f(log_path.c_str(), std::ios::binary | std::ios::app);
        if (f.is_open()) {
            f << error;
        }

        return (int)GetLastError();
    }

    return 0;
}

static std::string cmd_make_log_path_impl() {
    return make_temp_log_path();
}

static std::string cmd_read_log_impl(const std::string& log_path, bool use_powershell) {
    std::string raw = read_file_contents(log_path);

    if (raw.empty()) {
        return "";
    }

    return use_powershell ? raw : oem_to_utf8(raw);
}

static bool cmd_log_exists_impl(const std::string& log_path) {
    std::ifstream f(log_path.c_str(), std::ios::binary);
    return f.good();
}

static bool cmd_delete_log_impl(const std::string& log_path) {
    return std::remove(log_path.c_str()) == 0;
}

static std::string cmd_get_temp_path_impl() {
    return get_temp_path_impl();
}

// Debug helper.
// Shows what command will be sent to CreateProcessA.
// This no longer includes redirection, because stdout/stderr are assigned by handles.
static std::string cmd_debug_build_impl(const std::string& command, bool use_powershell) {
    return build_process_command(command, use_powershell);
}

static std::string as_cmd_run(const std::string& command, bool use_powershell) {
    return cmd_run_impl(command, use_powershell);
}

static int as_cmd_run_async(const std::string& command, const std::string& log_path, bool use_powershell) {
    return cmd_run_async_impl(command, log_path, use_powershell);
}

static std::string as_cmd_read_log(const std::string& log_path, bool use_powershell) {
    return cmd_read_log_impl(log_path, use_powershell);
}

static std::string as_cmd_make_log_path() {
    return cmd_make_log_path_impl();
}

static bool as_cmd_log_exists(const std::string& log_path) {
    return cmd_log_exists_impl(log_path);
}

static bool as_cmd_delete_log(const std::string& log_path) {
    return cmd_delete_log_impl(log_path);
}

static std::string as_cmd_get_temp_path() {
    return cmd_get_temp_path_impl();
}

static std::string as_cmd_debug_build(const std::string& command, bool use_powershell) {
    return cmd_debug_build_impl(command, use_powershell);
}

plugin_main(nvgt_plugin_shared* shared) {
    prepare_plugin(shared);

    asIScriptEngine* engine = shared->script_engine;

    engine->RegisterGlobalFunction("string cmd_run(const string &in command, bool use_powershell = false)", asFUNCTION(as_cmd_run), asCALL_CDECL);
    engine->RegisterGlobalFunction("int cmd_run_async(const string &in command, const string &in log_path, bool use_powershell = false)", asFUNCTION(as_cmd_run_async), asCALL_CDECL);
    engine->RegisterGlobalFunction("string cmd_read_log(const string &in log_path, bool use_powershell = false)", asFUNCTION(as_cmd_read_log), asCALL_CDECL);
    engine->RegisterGlobalFunction("bool cmd_log_exists(const string &in log_path)", asFUNCTION(as_cmd_log_exists), asCALL_CDECL);
    engine->RegisterGlobalFunction("bool cmd_delete_log(const string &in log_path)", asFUNCTION(as_cmd_delete_log), asCALL_CDECL);
    engine->RegisterGlobalFunction("string cmd_get_temp_path()", asFUNCTION(as_cmd_get_temp_path), asCALL_CDECL);
    engine->RegisterGlobalFunction("string cmd_make_log_path()", asFUNCTION(as_cmd_make_log_path), asCALL_CDECL);

    // Debug helper. Safe to leave in while testing.
    engine->RegisterGlobalFunction("string cmd_debug_build(const string &in command, bool use_powershell = false)", asFUNCTION(as_cmd_debug_build), asCALL_CDECL);

    return true;
}
