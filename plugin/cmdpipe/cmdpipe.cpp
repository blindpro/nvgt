#include <string>
#include <vector>
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
//  CmdPipe class
//  One instance = one hidden shell process with full stdin/stdout/stderr piping
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

    CmdPipe() : refcount(1), running(false) {
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

    // open(command) — spawn a hidden process. Pass "cmd.exe" for an interactive shell.
    bool open(const std::string& command) {
        if (running) close_process();
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
        running = true;
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
        running = true;
        return true;
#endif
    }

    // write(text) — raw write to stdin, no newline added
    bool write(const std::string& text) {
        if (!running || text.empty()) return false;
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(hStdin_Write, text.c_str(), (DWORD)text.size(), &written, NULL)
               && written == (DWORD)text.size();
#else
        return ::write(fd_stdin, text.c_str(), text.size()) == (ssize_t)text.size();
#endif
    }

    // writeline(text) — write text + newline. Use this to send commands.
    bool writeline(const std::string& text) {
#ifdef _WIN32
        return write(text + "\r\n");
#else
        return write(text + "\n");
#endif
    }

    // read_stdout(max_bytes) — non-blocking read. Returns "" if nothing available yet.
    std::string read_stdout(int max_bytes = 4096) {
        if (!running) return "";
#ifdef _WIN32
        return read_pipe(hStdout_Read, max_bytes);
#else
        return read_pipe(fd_stdout, max_bytes);
#endif
    }

    // read_stderr(max_bytes) — non-blocking read from stderr.
    std::string read_stderr(int max_bytes = 4096) {
        if (!running) return "";
#ifdef _WIN32
        return read_pipe(hStderr_Read, max_bytes);
#else
        return read_pipe(fd_stderr, max_bytes);
#endif
    }

    // read_stdout_wait(timeout_ms) — blocking read, waits up to timeout_ms milliseconds.
    std::string read_stdout_wait(int timeout_ms = 2000) {
        std::string result;
        int waited = 0;
        while (waited < timeout_ms) {
            std::string chunk = read_stdout(4096);
            if (!chunk.empty()) {
                result += chunk;
                waited = timeout_ms - 100;
            } else {
#ifdef _WIN32
                Sleep(10);
#else
                usleep(10000);
#endif
                waited += 10;
            }
        }
        return result;
    }

    // is_running() — returns true if the child process is still alive.
    bool is_running() {
        if (!running) return false;
#ifdef _WIN32
        DWORD code = 0;
        if (!GetExitCodeProcess(hProcess, &code)) { running = false; return false; }
        if (code != STILL_ACTIVE) { running = false; return false; }
        return true;
#else
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) > 0) { running = false; return false; }
        return true;
#endif
    }

    // get_exit_code() — returns exit code, or -1 if still running.
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

    // close_stdin() — close the stdin pipe, sending EOF to the child process.
    void close_stdin() {
#ifdef _WIN32
        if (hStdin_Write) { CloseHandle(hStdin_Write); hStdin_Write = NULL; }
#else
        if (fd_stdin >= 0) { ::close(fd_stdin); fd_stdin = -1; }
#endif
    }

    // close_process() — kill the process and clean up all handles.
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
#ifdef _WIN32
    HANDLE hProcess, hStdin_Write, hStdout_Read, hStderr_Read;
#else
    pid_t pid;
    int fd_stdin, fd_stdout, fd_stderr;
#endif

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
};

// ─────────────────────────────────────────────────────────────────────────────
//  Plugin entry point — matches the confirmed real NVGT plugin pattern exactly
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