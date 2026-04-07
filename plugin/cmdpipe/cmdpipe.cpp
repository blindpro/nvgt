#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <signal.h>
#endif
#include "../../src/nvgt_plugin.h"

// ─────────────────────────────────────────────────────────────────────────────
//  CmdPipe — persistent shell with sentinel-based output fencing
//
//  Design:
//    open(command)            — spawns the shell (e.g. "cmd.exe" or "powershell")
//    run(command, timeout_ms) — sends command, waits for sentinel, returns clean output
//    write/writeline          — raw access if you need it
//    read_stdout/read_stderr  — raw non-blocking reads
// ─────────────────────────────────────────────────────────────────────────────
class CmdPipe {
public:
    int refcount;
    void AddRef()  { refcount++; }
    void Release() { if (--refcount == 0) delete this; }

    static CmdPipe* Factory() {
        CmdPipe* obj = new CmdPipe();
        obj->refcount = 1;
        return obj;
    }

    CmdPipe() : refcount(1), running(false), is_powershell(false), sentinel_counter(0) {
#ifdef _WIN32
        hProcess     = NULL;
        hStdin_Write = NULL;
        hStdout_Read = NULL;
        hStderr_Read = NULL;
#else
        pid       = -1;
        fd_stdin  = -1;
        fd_stdout = -1;
        fd_stderr = -1;
#endif
    }

    ~CmdPipe() { close_process(); }

    // ── open ──────────────────────────────────────────────────────────────────
    // Spawn the shell.  Detects powershell/pwsh automatically to adjust
    // sentinel syntax.  Also sends an initial "echo off" for cmd.exe so
    // prompts and command echoes are suppressed from the very first command.
    bool open(const std::string& command) {
        if (running) close_process();

        // Detect shell type for sentinel command syntax
        std::string lower = command;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        is_powershell = (lower.find("powershell") != std::string::npos ||
                         lower.find("pwsh")        != std::string::npos);

        if (!spawn(command)) return false;
        running = true;

        if (!is_powershell) {
            // Turn off command echo for cmd.exe — discard the startup banner
            raw_writeline("@echo off");
            drain_until_quiet(500);
        } else {
            // PowerShell: suppress its startup banner by just draining it
            drain_until_quiet(800);
        }
        return true;
    }

    // ── run ───────────────────────────────────────────────────────────────────
    // Send one command, block until its output is complete, return clean output.
    // timeout_ms is the maximum time to wait for the sentinel to appear.
    std::string run(const std::string& command, int timeout_ms = 5000) {
        if (!running) return "";

        // Build a unique sentinel string
        std::string sentinel = make_sentinel();

        // Send the user's command, then immediately send the sentinel command.
        // For cmd.exe:   echo __NVGT_DONE_12345__
        // For PowerShell: Write-Host "__NVGT_DONE_12345__"
        raw_writeline(command);
        if (is_powershell)
            raw_writeline("Write-Host '" + sentinel + "'");
        else
            raw_writeline("echo " + sentinel);

        // Read until we see the sentinel line, then return everything before it
        return read_until_sentinel(sentinel, timeout_ms);
    }

    // ── raw write access ──────────────────────────────────────────────────────
    bool write(const std::string& text) {
        if (!running || text.empty()) return false;
        return raw_write(text);
    }

    bool writeline(const std::string& text) {
#ifdef _WIN32
        return write(text + "\r\n");
#else
        return write(text + "\n");
#endif
    }

    // ── raw non-blocking reads ─────────────────────────────────────────────────
    std::string read_stdout(int max_bytes = 4096) {
        if (!running) return "";
#ifdef _WIN32
        return read_pipe(hStdout_Read, max_bytes);
#else
        return read_pipe(fd_stdout, max_bytes);
#endif
    }

    std::string read_stderr(int max_bytes = 4096) {
        if (!running) return "";
#ifdef _WIN32
        return read_pipe(hStderr_Read, max_bytes);
#else
        return read_pipe(fd_stderr, max_bytes);
#endif
    }

    // ── blocking read (legacy helper) ─────────────────────────────────────────
    std::string read_stdout_wait(int timeout_ms = 2000) {
        std::string result;
        int waited = 0;
        while (waited < timeout_ms) {
            std::string chunk = read_stdout(4096);
            if (!chunk.empty()) {
                result += chunk;
                waited = timeout_ms - 100;   // got data; allow 100 ms more to drain
            } else {
                sleep_ms(10);
                waited += 10;
            }
        }
        return result;
    }

    // ── process state ─────────────────────────────────────────────────────────
    bool is_running() {
        if (!running) return false;
#ifdef _WIN32
        DWORD code = 0;
        if (!GetExitCodeProcess(hProcess, &code)) { running = false; return false; }
        if (code != STILL_ACTIVE)                 { running = false; return false; }
        return true;
#else
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) > 0) { running = false; return false; }
        return true;
#endif
    }

    int get_exit_code() {
#ifdef _WIN32
        if (!hProcess) return -1;
        DWORD code = 0;
        GetExitCodeProcess(hProcess, &code);
        return (code == STILL_ACTIVE) ? -1 : (int)code;
#else
        if (pid < 0) return -1;
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r <= 0) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    void close_stdin() {
#ifdef _WIN32
        if (hStdin_Write) { CloseHandle(hStdin_Write); hStdin_Write = NULL; }
#else
        if (fd_stdin >= 0) { ::close(fd_stdin); fd_stdin = -1; }
#endif
    }

    void close_process() {
        if (!running) return;
        running = false;
#ifdef _WIN32
        if (hProcess)     { TerminateProcess(hProcess, 0); WaitForSingleObject(hProcess, 1000); CloseHandle(hProcess); hProcess = NULL; }
        if (hStdin_Write) { CloseHandle(hStdin_Write); hStdin_Write = NULL; }
        if (hStdout_Read) { CloseHandle(hStdout_Read); hStdout_Read = NULL; }
        if (hStderr_Read) { CloseHandle(hStderr_Read); hStderr_Read = NULL; }
#else
        if (pid > 0)        { kill(pid, SIGTERM); int s; waitpid(pid, &s, 0); pid = -1; }
        if (fd_stdin  >= 0) { ::close(fd_stdin);  fd_stdin  = -1; }
        if (fd_stdout >= 0) { ::close(fd_stdout); fd_stdout = -1; }
        if (fd_stderr >= 0) { ::close(fd_stderr); fd_stderr = -1; }
#endif
    }

private:
    bool running;
    bool is_powershell;
    int  sentinel_counter;

#ifdef _WIN32
    HANDLE hProcess, hStdin_Write, hStdout_Read, hStderr_Read;
#else
    pid_t pid;
    int fd_stdin, fd_stdout, fd_stderr;
#endif

    // ── sentinel generation ────────────────────────────────────────────────────
    // Each call to run() gets a unique marker so stale output from a previous
    // command can never be mistaken for the end of the current one.
    std::string make_sentinel() {
        return "__NVGT_DONE_" + std::to_string(++sentinel_counter) + "__";
    }

    // ── read until sentinel ────────────────────────────────────────────────────
    // Accumulates stdout until the sentinel line appears, then returns
    // everything before it with prompts and blank leading/trailing lines stripped.
    std::string read_until_sentinel(const std::string& sentinel, int timeout_ms) {
        std::string accumulated;
        int waited = 0;
        const int poll_interval = 10;   // ms between polls

        while (waited < timeout_ms) {
            std::string chunk = read_stdout(8192);
            if (!chunk.empty()) {
                accumulated += chunk;
                // Check whether the sentinel has arrived yet
                std::string::size_type pos = accumulated.find(sentinel);
                if (pos != std::string::npos) {
                    // Keep only what came before the sentinel line
                    std::string before = accumulated.substr(0, pos);
                    return clean_output(before);
                }
                // Got data but not the sentinel yet — reset idle timer
                waited = 0;
            } else {
                sleep_ms(poll_interval);
                waited += poll_interval;
            }
        }
        // Timed out — return whatever we have (minus any partial sentinel)
        return clean_output(accumulated);
    }

    // ── clean_output ──────────────────────────────────────────────────────────
    // Removes:
    //   • cmd.exe / PowerShell prompt lines  (e.g. "C:\Users\foo>", "PS C:\>")
    //   • Blank lines at the top and bottom
    //   • Windows \r characters
    std::string clean_output(const std::string& raw) {
        // Normalise line endings
        std::string text;
        text.reserve(raw.size());
        for (char c : raw) {
            if (c != '\r') text += c;
        }

        // Split into lines, filter prompt lines, rejoin
        std::vector<std::string> lines;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (is_prompt_line(line)) continue;
            lines.push_back(line);
        }

        // Strip leading blank lines
        while (!lines.empty() && lines.front().empty())
            lines.erase(lines.begin());

        // Strip trailing blank lines
        while (!lines.empty() && lines.back().empty())
            lines.pop_back();

        std::string result;
        for (size_t i = 0; i < lines.size(); ++i) {
            result += lines[i];
            if (i + 1 < lines.size()) result += '\n';
        }
        return result;
    }

    // Returns true for lines that are shell prompts we should discard
    bool is_prompt_line(const std::string& line) {
        if (line.empty()) return false;

        // cmd.exe prompt: ends with '>' possibly preceded by a path
        // e.g. "C:\Users\Bob>" or "C:\>"
        if (!is_powershell) {
            // A prompt line contains '>' as the last non-space char and a drive letter
            size_t gt = line.rfind('>');
            if (gt != std::string::npos && gt + 1 == line.size()) {
                // Looks like a prompt — must have at least a letter + colon before it
                if (gt >= 2 && line[1] == ':') return true;
                // Or just bare ">"
                if (gt == 0) return true;
            }
            return false;
        } else {
            // PowerShell prompt: starts with "PS " or just "PS>"
            if (line.size() >= 3 && line[0]=='P' && line[1]=='S' &&
                (line[2]==' ' || line[2]=='>')) return true;
            return false;
        }
    }

    // ── drain helper (used at startup) ────────────────────────────────────────
    // Reads and discards output until the pipe is quiet for quiet_ms milliseconds
    void drain_until_quiet(int quiet_ms) {
        int silent = 0;
        while (silent < quiet_ms) {
            std::string chunk = read_stdout(8192);
            if (!chunk.empty()) { silent = 0; }
            else { sleep_ms(20); silent += 20; }
        }
        // Also drain stderr
        while (!read_stderr(8192).empty()) {}
    }

    // ── portable sleep ────────────────────────────────────────────────────────
    static void sleep_ms(int ms) {
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        usleep((useconds_t)ms * 1000);
#endif
    }

    // ── raw write ─────────────────────────────────────────────────────────────
    bool raw_write(const std::string& text) {
        if (text.empty()) return false;
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(hStdin_Write, text.c_str(), (DWORD)text.size(), &written, NULL)
               && written == (DWORD)text.size();
#else
        return ::write(fd_stdin, text.c_str(), text.size()) == (ssize_t)text.size();
#endif
    }

    bool raw_writeline(const std::string& text) {
#ifdef _WIN32
        return raw_write(text + "\r\n");
#else
        return raw_write(text + "\n");
#endif
    }

    // ── read_pipe (platform-specific non-blocking read) ───────────────────────
#ifdef _WIN32
    std::string read_pipe(HANDLE h, int max_bytes) {
        if (!h) return "";
        DWORD available = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) || available == 0) return "";
        if ((int)available > max_bytes) available = (DWORD)max_bytes;
        std::string buf(available, '\0');
        DWORD read_bytes = 0;
        if (!ReadFile(h, &buf[0], available, &read_bytes, NULL)) return "";
        buf.resize(read_bytes);
        return buf;
    }
#else
    std::string read_pipe(int fd, int max_bytes) {
        if (fd < 0) return "";
        std::string buf(max_bytes, '\0');
        ssize_t n = ::read(fd, &buf[0], max_bytes);
        if (n <= 0) return "";
        buf.resize(n);
        return buf;
    }
#endif

    // ── spawn (platform-specific process creation) ────────────────────────────
    bool spawn(const std::string& command) {
#ifdef _WIN32
        HANDLE hStdin_Read   = NULL;
        HANDLE hStdout_Write = NULL;
        HANDLE hStderr_Write = NULL;

        SECURITY_ATTRIBUTES sa;
        sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle       = TRUE;
        sa.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&hStdin_Read, &hStdin_Write, &sa, 0)) return false;
        SetHandleInformation(hStdin_Write, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&hStdout_Read, &hStdout_Write, &sa, 0)) {
            CloseHandle(hStdin_Read); CloseHandle(hStdin_Write); hStdin_Write = NULL;
            return false;
        }
        SetHandleInformation(hStdout_Read, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&hStderr_Read, &hStderr_Write, &sa, 0)) {
            CloseHandle(hStdin_Read);  CloseHandle(hStdin_Write);  hStdin_Write = NULL;
            CloseHandle(hStdout_Read); CloseHandle(hStdout_Write); hStdout_Read = NULL;
            return false;
        }
        SetHandleInformation(hStderr_Read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb          = sizeof(si);
        si.hStdInput   = hStdin_Read;
        si.hStdOutput  = hStdout_Write;
        si.hStdError   = hStderr_Write;
        si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        std::vector<char> cmdline(command.begin(), command.end());
        cmdline.push_back('\0');

        BOOL ok = CreateProcessA(NULL, cmdline.data(), NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);

        CloseHandle(hStdin_Read);
        CloseHandle(hStdout_Write);
        CloseHandle(hStderr_Write);

        if (!ok) {
            CloseHandle(hStdin_Write);  hStdin_Write = NULL;
            CloseHandle(hStdout_Read);  hStdout_Read = NULL;
            CloseHandle(hStderr_Read);  hStderr_Read = NULL;
            return false;
        }
        hProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
#else
        int pipe_stdin[2], pipe_stdout[2], pipe_stderr[2];
        if (pipe(pipe_stdin)  < 0) return false;
        if (pipe(pipe_stdout) < 0) { close(pipe_stdin[0]);  close(pipe_stdin[1]);  return false; }
        if (pipe(pipe_stderr) < 0) { close(pipe_stdin[0]);  close(pipe_stdin[1]);
                                     close(pipe_stdout[0]); close(pipe_stdout[1]); return false; }
        pid = fork();
        if (pid < 0) {
            close(pipe_stdin[0]);  close(pipe_stdin[1]);
            close(pipe_stdout[0]); close(pipe_stdout[1]);
            close(pipe_stderr[0]); close(pipe_stderr[1]);
            return false;
        }
        if (pid == 0) {
            dup2(pipe_stdin[0],  STDIN_FILENO);
            dup2(pipe_stdout[1], STDOUT_FILENO);
            dup2(pipe_stderr[1], STDERR_FILENO);
            close(pipe_stdin[0]);  close(pipe_stdin[1]);
            close(pipe_stdout[0]); close(pipe_stdout[1]);
            close(pipe_stderr[0]); close(pipe_stderr[1]);
            execl("/bin/sh", "sh", "-c", command.c_str(), (char*)NULL);
            _exit(127);
        }
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);
        fd_stdin  = pipe_stdin[1];
        fd_stdout = pipe_stdout[0];
        fd_stderr = pipe_stderr[0];
        fcntl(fd_stdout, F_SETFL, O_NONBLOCK);
        fcntl(fd_stderr, F_SETFL, O_NONBLOCK);
        return true;
#endif
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Plugin registration
// ─────────────────────────────────────────────────────────────────────────────
plugin_main(nvgt_plugin_shared* shared) {
    prepare_plugin(shared);
    asIScriptEngine* engine = shared->script_engine;

    engine->RegisterObjectType("cmd_pipe", 0, asOBJ_REF);

    engine->RegisterObjectBehaviour("cmd_pipe", asBEHAVE_FACTORY,
        "cmd_pipe@ f()", asFUNCTION(CmdPipe::Factory), asCALL_CDECL);
    engine->RegisterObjectBehaviour("cmd_pipe", asBEHAVE_ADDREF,
        "void f()", asMETHOD(CmdPipe, AddRef), asCALL_THISCALL);
    engine->RegisterObjectBehaviour("cmd_pipe", asBEHAVE_RELEASE,
        "void f()", asMETHOD(CmdPipe, Release), asCALL_THISCALL);

    engine->RegisterObjectMethod("cmd_pipe",
        "bool open(const string&in command)",
        asMETHOD(CmdPipe, open), asCALL_THISCALL);

    // ★ The main new method — use this instead of writeline + read_stdout_wait
    engine->RegisterObjectMethod("cmd_pipe",
        "string run(const string&in command, int timeout_ms = 5000)",
        asMETHOD(CmdPipe, run), asCALL_THISCALL);

    engine->RegisterObjectMethod("cmd_pipe",
        "bool write(const string&in text)",
        asMETHOD(CmdPipe, write), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "bool writeline(const string&in text)",
        asMETHOD(CmdPipe, writeline), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "string read_stdout(int max_bytes = 4096)",
        asMETHOD(CmdPipe, read_stdout), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "string read_stderr(int max_bytes = 4096)",
        asMETHOD(CmdPipe, read_stderr), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "string read_stdout_wait(int timeout_ms = 2000)",
        asMETHOD(CmdPipe, read_stdout_wait), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "bool is_running()",
        asMETHOD(CmdPipe, is_running), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "int get_exit_code()",
        asMETHOD(CmdPipe, get_exit_code), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "void close_stdin()",
        asMETHOD(CmdPipe, close_stdin), asCALL_THISCALL);
    engine->RegisterObjectMethod("cmd_pipe",
        "void close_process()",
        asMETHOD(CmdPipe, close_process), asCALL_THISCALL);

    return true;
}