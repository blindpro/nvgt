# cmdpipe — NVGT Plugin
Interactive hidden CMD/shell pipe for NVGT scripts.

Spawn a hidden CMD.exe (or any process), send commands in,
and read stdout/stderr back — all without any visible window.

---

## What you get

| Method | What it does |
|---|---|
| `open(command)` | Spawn a hidden process (e.g. `"cmd.exe"`) |
| `writeline(text)` | Send a command (adds newline automatically) |
| `write(text)` | Raw write to stdin, no newline |
| `read_stdout()` | Non-blocking read — returns whatever is available NOW |
| `read_stdout_wait(ms)` | Blocking read — waits up to `ms` milliseconds |
| `read_stderr()` | Non-blocking read from stderr |
| `is_running()` | True if process is still alive |
| `get_exit_code()` | Returns exit code, or -1 if still running |
| `close_stdin()` | Send EOF to stdin (for programs that wait for EOF) |
| `close_process()` | Kill the process and clean up |

---

## Quick NVGT usage

```angelscript
plugin_load("cmdpipe");

void main() {
    cmd_pipe@ shell = cmd_pipe();
    shell.open("cmd.exe");
    wait(400);
    shell.read_stdout(4096); // discard CMD banner

    shell.writeline("dir C:\\");
    string output = shell.read_stdout_wait(2000);
    speak(output);

    shell.close_process();
}
```

---

## Build instructions (Windows)

You only need to do this once. After building you get `cmdpipe.dll`
which you just drop next to your `.nvgt` script file.

### Step 1 — Install Python (if you don't have it)
Download from https://python.org — tick "Add Python to PATH" during install.

### Step 2 — Install SCons
Open a normal Command Prompt and run:
```
pip install scons
```

### Step 3 — Install Visual Studio Build Tools (free)
Download "Build Tools for Visual Studio 2022" from:
https://visualstudio.microsoft.com/downloads/
→ Scroll down to "Tools for Visual Studio" → "Build Tools for Visual Studio 2022"

During install, tick: **"Desktop development with C++"**
(You do NOT need the full Visual Studio IDE — just the build tools.)

### Step 4 — Get NVGT source + dev libraries
```
git clone https://github.com/samtupy/nvgt.git
cd nvgt
```
Then go to the NVGT GitHub Releases page:
https://github.com/samtupy/nvgt/releases
Download `windev.zip` and extract it INTO the nvgt folder so you have:
`nvgt/windev/include/...`

### Step 5 — Copy this plugin folder into NVGT
Copy the entire `cmdpipe` folder into `nvgt/plugin/`:
```
nvgt/
  plugin/
    cmdpipe/
      cmdpipe.cpp      ← C++ source
      _SConscript      ← build script
      README.md        ← this file
```

### Step 6 — Build!
Open the "Developer Command Prompt for VS 2022" (search in Start Menu),
navigate to the nvgt folder, and run:
```
scons -s
```
This builds everything. Your plugin will be at:
`nvgt/lib/cmdpipe.dll`

### Step 7 — Use it
Copy `cmdpipe.dll` to the same folder as your `.nvgt` script.
Then in your script:
```angelscript
plugin_load("cmdpipe");
```

---

## Build on Linux / Mac

Same steps but:
- Use `g++` or `clang++` (usually pre-installed or available via package manager)
- No Visual Studio needed
- Output will be `cmdpipe.so` (Linux) or `cmdpipe.dylib` (Mac)
- The plugin uses POSIX fork/exec/pipe — no extra libraries needed

---

## Tips

**Discarding the CMD banner:**
When you `open("cmd.exe")`, CMD prints a startup banner. Always do this after opening:
```angelscript
wait(400);
shell.read_stdout(4096); // throw away the banner
```

**Parsing output:**
Split on newlines to process line by line:
```angelscript
string[] lines = output.split("\n");
for (uint i = 0; i < lines.length(); i++) {
    string line = lines[i].trim();
    if (line.find("something") >= 0)
        speak(line);
}
```

**Non-blocking vs blocking reads:**
- Use `read_stdout()` inside game loops — it returns instantly with whatever is available.
- Use `read_stdout_wait(ms)` for one-shot commands where you want the full result.

**Works with anything, not just CMD:**
```angelscript
shell.open("powershell.exe -NoProfile");
shell.open("python.exe");
shell.open("ping 8.8.8.8 -t"); // live streaming output!
```
