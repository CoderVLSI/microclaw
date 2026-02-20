#include "minos.h"
#include <vector>

static String shell_output;
static String s_cwd = "/";

static void shell_print(const String &s) { shell_output += s; }
static void shell_println(const String &s) { shell_output += s + "\n"; }

/* Helper to resolve paths based on CWD */
static String resolve_path(const String &path) {
    if (path.length() == 0) return s_cwd;
    if (path.startsWith("/")) return path;
    String res = s_cwd;
    if (!res.endsWith("/")) res += "/";
    res += path;
    return res;
}

/* Command Implementations */
static void cmd_help() {
    shell_println("\nAvailable commands:");
    shell_println("  help       - Show this help");
    shell_println("  pwd        - Print working directory");
    shell_println("  cd <dir>   - Change directory");
    shell_println("  ls         - List files");
    shell_println("  ps         - List tasks (alias: top)");
    shell_println("  cat <file> - Print file content");
    shell_println("  touch <f>  - Create empty file");
    shell_println("  mkdir <d>  - Create directory (simulated)");
    shell_println("  rm <file>  - Delete a file");
    shell_println("  df         - Show disk usage");
    shell_println("  free       - Show free RAM");
    shell_println("  uptime     - Show system uptime");
    shell_println("  sysinfo    - System info (alias: uname)");
    shell_println("  reboot     - Restart ESP32");
}

static void cmd_ps() {
    shell_println("\nPID  STATE      PRI  NAME");
    shell_println("---  ---------  ---  ----");
    for (uint32_t i = 0; i < task_count; i++) {
        const char *state = "UNKNOWN";
        switch (tasks[i].state) {
            case TASK_READY:    state = "READY"; break;
            case TASK_RUNNING:  state = "RUNNING"; break;
            case TASK_SLEEPING: state = "SLEEP"; break;
            case TASK_BLOCKED:  state = "BLOCKED"; break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%-3d  %-9s  %-3d  %s", 
                tasks[i].id, state, tasks[i].priority, tasks[i].name);
        shell_println(buf);
    }
}

static void cmd_ls(const String &path) {
    String p = resolve_path(path);
    shell_println("\nListing " + p + ":");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file){
        String fname = String(file.name());
        if (fname.startsWith(p) || p == "/") {
            shell_print(fname + " \t");
            shell_println(String(file.size()) + " bytes");
        }
        file = root.openNextFile();
    }
}

static void cmd_cat(const String &path) {
    String p = resolve_path(path);
    if (!SPIFFS.exists(p)) {
        shell_println("Error: File " + p + " not found");
        return;
    }
    File f = SPIFFS.open(p, "r");
    while(f.available()) {
        shell_print(String((char)f.read()));
    }
    shell_println("");
    f.close();
}

static void cmd_touch(const String &path) {
    String p = resolve_path(path);
    File f = SPIFFS.open(p, "w");
    if (f) {
        shell_println("Created " + p);
        f.close();
    } else {
        shell_println("Error: Could not create " + p);
    }
}

static void cmd_mkdir(const String &path) {
    String p = resolve_path(path);
    if (!p.endsWith("/")) p += "/";
    p += ".keep";
    File f = SPIFFS.open(p, "w");
    if (f) {
        shell_println("Created directory " + path);
        f.close();
    } else {
        shell_println("Error: Could not create directory " + path);
    }
}

static void cmd_rm(const String &path) {
    String p = resolve_path(path);
    if (SPIFFS.remove(p)) {
        shell_println("Removed " + p);
    } else {
        shell_println("Error: Could not remove " + p);
    }
}

static void cmd_df() {
    size_t total = SPIFFS.totalBytes();
    size_t used = SPIFFS.usedBytes();
    shell_println("SPIFFS Usage:");
    shell_println("Total: " + String(total) + " bytes");
    shell_println("Used:  " + String(used) + " bytes");
    shell_println("Free:  " + String(total - used) + " bytes");
}

static void cmd_free() {
    shell_println("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
}

void shell_init(void) {
    shell_println("MinOS Shell v0.3 (Linux-Lite Port)");
    shell_println("Type 'help' for commands");
    s_cwd = "/";
}

void shell_run_once(const String &input, String &output) {
    shell_output = "";
    String cmd_line = input;
    cmd_line.trim();

    if (cmd_line == "help") cmd_help();
    else if (cmd_line == "pwd") shell_println(s_cwd);
    else if (cmd_line == "ps" || cmd_line == "top") cmd_ps();
    else if (cmd_line == "ls") cmd_ls("");
    else if (cmd_line.startsWith("ls ")) cmd_ls(cmd_line.substring(3));
    else if (cmd_line == "df") cmd_df();
    else if (cmd_line == "free") cmd_free();
    else if (cmd_line == "uptime") {
        uint32_t sec = millis() / 1000;
        uint32_t min = sec / 60;
        uint32_t hr = min / 60;
        shell_println("Uptime: " + String(hr) + "h " + String(min % 60) + "m " + String(sec % 60) + "s");
    }
    else if (cmd_line == "sysinfo" || cmd_line == "uname") {
        shell_println("OS: MinOS v0.3 (Linux-Lite)");
        shell_println("CPU: Xtensa LX6 @ 240MHz");
        shell_println("Flash: " + String(ESP.getFlashChipSize() / (1024*1024)) + "MB");
        shell_println("Chip ID: " + String((uint32_t)ESP.getEfuseMac(), HEX));
    }
    else if (cmd_line == "reboot") {
        shell_println("Rebooting...");
        output = shell_output;
        delay(100);
        ESP.restart();
    }
    else if (cmd_line.startsWith("cd ")) {
        String path = cmd_line.substring(3);
        path.trim();
        if (path == "..") {
            if (s_cwd != "/") {
                int last = s_cwd.lastIndexOf('/', s_cwd.length() - 2);
                if (last < 0) s_cwd = "/";
                else s_cwd = s_cwd.substring(0, last + 1);
            }
        } else if (path == ".") {
            // Nothing
        } else {
            s_cwd = resolve_path(path);
            if (!s_cwd.endsWith("/")) s_cwd += "/";
        }
    }
    else if (cmd_line.startsWith("cat ")) {
        cmd_cat(cmd_line.substring(4));
    }
    else if (cmd_line.startsWith("touch ")) {
        cmd_touch(cmd_line.substring(6));
    }
    else if (cmd_line.startsWith("mkdir ")) {
        cmd_mkdir(cmd_line.substring(6));
    }
    else if (cmd_line.startsWith("rm ")) {
        cmd_rm(cmd_line.substring(3));
    }
    else if (cmd_line.length() > 0) {
        shell_println("MinOS: Unknown command: " + cmd_line);
    }

    output = shell_output;
}
