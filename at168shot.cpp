// AT168Shot - Screenshot capture tool for AnyTone 168 radio displays
// Cross-platform C++ + Dear ImGui + GLFW implementation

// Platform detection
#if defined(_WIN32)
    #define RADSHOT_WINDOWS
#elif defined(__APPLE__)
    #define RADSHOT_MACOS
#else
    #define RADSHOT_LINUX
#endif

#ifdef RADSHOT_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <setupapi.h>
    #include <shlobj.h>
#else
    #include <termios.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <dirent.h>
    #include <sys/stat.h>
    #include <errno.h>
#endif

#ifdef RADSHOT_MACOS
    #include <CoreFoundation/CoreFoundation.h>
    #include <IOKit/serial/ioss.h>
    #include <sys/ioctl.h>
#endif

#include <GLFW/glfw3.h>
#ifdef RADSHOT_WINDOWS
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

// The Windows SDK ships an OpenGL 1.1 gl.h, so GL 1.2+ tokens are missing.
// Every driver we care about supports them at runtime; just declare them.
#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "tinyfiledialogs.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// =============================================================================
// Constants
// =============================================================================

constexpr const char* APP_VERSION = "0.1";

// The radio answers the "SCR" command with one raw 16-bit RGB565 (little
// endian) framebuffer dump. The scanlines come out transposed relative to what
// the display shows: RAW_WIDTH pixels per row, RAW_HEIGHT rows. Rotating (and
// optionally mirroring) that recovers the 128x160 portrait image.
constexpr int RAW_WIDTH = 160;
constexpr int RAW_HEIGHT = 128;
constexpr int DISPLAY_WIDTH = 128;
constexpr int DISPLAY_HEIGHT = 160;
constexpr int FRAME_SIZE = RAW_WIDTH * RAW_HEIGHT * 2;  // 40960 bytes
// The AT-D168UV runs its program-mode serial link at 921600 (at168-cps and the
// at168-flasher tools both do); 115200 is what qdmr uses for other AnyTone
// radios, and 4 Mbaud is the firmware-update/license speed. Which one the
// screenshot firmware answers on is a property of that firmware, so the rate is
// selectable and remembered rather than compiled in.
constexpr int BAUD_RATES[] = { 921600, 115200, 230400, 460800, 4000000 };
constexpr int BAUD_RATE_COUNT = (int)(sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));
constexpr int DEFAULT_BAUD_INDEX = 0;
constexpr uint8_t SCREENSHOT_CMD[] = { 'S', 'C', 'R' };
constexpr int PREVIEW_SCALE = 3;
constexpr int THUMB_DIVISOR = 2;
constexpr int GALLERY_COLUMNS = 6;
// Give up when the radio has sent nothing for this long. Time based, not a
// frame count: the read loop runs once per rendered frame, so a frame-based
// limit changes meaning with the refresh rate and stalls when the window is
// occluded and rendering slows down.
constexpr double CAPTURE_QUIET_TIMEOUT_S = 5.0;
constexpr int READS_PER_FRAME = 64;
constexpr int PATH_MAX_LEN = 4096;

// =============================================================================
// DPI scaling
// =============================================================================

// Content scale of the monitor the window is on (1.0 = 96 DPI, 1.5 = 150%, ...).
// Multiply hardcoded pixel sizes by this so the UI keeps its physical size.
static float g_dpi_scale = 1.0f;

// Saved window sizes are already in pixels, so only the built-in default gets
// scaled up for the monitor.
static bool g_have_saved_size = false;

// Style at scale 1.0. ScaleAllSizes() compounds, so rescaling has to start here.
static ImGuiStyle g_base_style;

// Virtual monitors from some accessibility tools report a scale of 0, and the
// backend passes that through, so clamp every scale on the way in.
static float QueryDpiScale(GLFWwindow* window) {
    float scale = ImGui_ImplGlfw_GetContentScaleForWindow(window);
    return (scale > 0.0f) ? scale : 1.0f;
}

static void ApplyDpiScale(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;  // re-rasterizes the font at the new size
    g_dpi_scale = scale;
}

// Orientation applied to the raw frame. Rotation is clockwise, the mirror is
// applied afterwards; these four combinations are the only transforms that turn
// a 160x128 frame into a 128x160 one, so one of them is always the right answer.
enum Rotation { ROT_CW_90 = 0, ROT_CCW_90 = 1 };
constexpr int DEFAULT_ROTATION = ROT_CW_90;
constexpr bool DEFAULT_MIRROR = true;   // rotate + mirror == transpose

// =============================================================================
// Screenshot Structure
// =============================================================================

struct Timestamp {
    int year, month, day, hour, minute, second;
};

struct Screenshot {
    int id;
    char name[256];
    uint8_t* raw_frame;     // FRAME_SIZE bytes, RGB565 little endian, as received
    uint8_t* rgba_preview;  // DISPLAY_WIDTH*PREVIEW_SCALE x DISPLAY_HEIGHT*PREVIEW_SCALE x 4
    uint8_t* rgba_thumb;    // DISPLAY_WIDTH x DISPLAY_HEIGHT x 4
    GLuint texture_preview;
    GLuint texture_thumb;
    Timestamp timestamp;

    Screenshot() : id(0), raw_frame(nullptr), rgba_preview(nullptr), rgba_thumb(nullptr),
                   texture_preview(0), texture_thumb(0) {
        name[0] = 0;
        memset(&timestamp, 0, sizeof(timestamp));
    }

    ~Screenshot() {
        if (raw_frame) delete[] raw_frame;
        if (rgba_preview) delete[] rgba_preview;
        if (rgba_thumb) delete[] rgba_thumb;
        if (texture_preview) glDeleteTextures(1, &texture_preview);
        if (texture_thumb) glDeleteTextures(1, &texture_thumb);
    }

    // Prevent copying
    Screenshot(const Screenshot&) = delete;
    Screenshot& operator=(const Screenshot&) = delete;

    // Allow moving
    Screenshot(Screenshot&& other) noexcept {
        id = other.id;
        strcpy(name, other.name);
        raw_frame = other.raw_frame;
        rgba_preview = other.rgba_preview;
        rgba_thumb = other.rgba_thumb;
        texture_preview = other.texture_preview;
        texture_thumb = other.texture_thumb;
        timestamp = other.timestamp;
        other.raw_frame = nullptr;
        other.rgba_preview = nullptr;
        other.rgba_thumb = nullptr;
        other.texture_preview = 0;
        other.texture_thumb = 0;
    }

    Screenshot& operator=(Screenshot&& other) noexcept {
        if (this != &other) {
            if (raw_frame) delete[] raw_frame;
            if (rgba_preview) delete[] rgba_preview;
            if (rgba_thumb) delete[] rgba_thumb;
            if (texture_preview) glDeleteTextures(1, &texture_preview);
            if (texture_thumb) glDeleteTextures(1, &texture_thumb);

            id = other.id;
            strcpy(name, other.name);
            raw_frame = other.raw_frame;
            rgba_preview = other.rgba_preview;
            rgba_thumb = other.rgba_thumb;
            texture_preview = other.texture_preview;
            texture_thumb = other.texture_thumb;
            timestamp = other.timestamp;
            other.raw_frame = nullptr;
            other.rgba_preview = nullptr;
            other.rgba_thumb = nullptr;
            other.texture_preview = 0;
            other.texture_thumb = 0;
        }
        return *this;
    }
};

// =============================================================================
// Application State
// =============================================================================

struct AppState {
    // Window
    GLFWwindow* window = nullptr;
    int window_width = 820;
    int window_height = 900;
    bool running = true;

    // Serial
    std::vector<std::string> com_ports;
    int selected_port = -1;
#ifdef RADSHOT_WINDOWS
    HANDLE serial_handle = INVALID_HANDLE_VALUE;
#else
    int serial_fd = -1;
#endif
    bool is_connected = false;
    int baud_index = DEFAULT_BAUD_INDEX;
    char status_message[256] = "Disconnected";

    // Capture
    bool is_capturing = false;
    int capture_progress = 0;
    uint8_t capture_buffer[FRAME_SIZE];
    int capture_bytes = 0;
    double capture_last_data = 0.0;
    bool line_was_busy = false;   // radio was still talking when SCR went out

    // Frame orientation (see Rotation)
    int rotation = DEFAULT_ROTATION;
    bool mirror = DEFAULT_MIRROR;

    // Screenshots
    std::vector<Screenshot*> screenshots;
    int next_id = 1;
    int selected_screenshot = -1;

    // UI
    char rename_buffer[256] = {0};
    bool show_delete_popup = false;
    bool show_clear_popup = false;
    bool show_exit_popup = false;
    bool pending_close = false;

    // Settings persistence
    char last_save_directory[PATH_MAX_LEN] = {0};
    char last_port_name[256] = {0};
    int window_x = -1;
    int window_y = -1;
};

static AppState g_state;

// =============================================================================
// Settings Persistence
// =============================================================================

static void GetSettingsPath(char* path, size_t pathSize) {
#ifdef RADSHOT_WINDOWS
    char exePath[PATH_MAX_LEN];
    GetModuleFileNameA(nullptr, exePath, PATH_MAX_LEN);
    char* dot = strrchr(exePath, '.');
    if (dot) {
        strcpy(dot, ".ini");
    } else {
        strcat(exePath, ".ini");
    }
    strncpy(path, exePath, pathSize - 1);
    path[pathSize - 1] = 0;
#elif defined(RADSHOT_MACOS)
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/at168shot", home);
    mkdir(dir, 0755);
    snprintf(path, pathSize, "%s/at168shot.ini", dir);
#else
    const char* configHome = getenv("XDG_CONFIG_HOME");
    char dir[PATH_MAX_LEN];
    if (configHome && configHome[0]) {
        snprintf(dir, sizeof(dir), "%s/at168shot", configHome);
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(dir, sizeof(dir), "%s/.config/at168shot", home);
    }
    mkdir(dir, 0755);
    snprintf(path, pathSize, "%s/at168shot.ini", dir);
#endif
}

static void LoadSettings() {
    char iniPath[PATH_MAX_LEN];
    GetSettingsPath(iniPath, sizeof(iniPath));

    FILE* f = fopen(iniPath, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* value = eq + 1;

        if (strcmp(key, "window_x") == 0) {
            g_state.window_x = atoi(value);
        } else if (strcmp(key, "window_y") == 0) {
            g_state.window_y = atoi(value);
        } else if (strcmp(key, "window_width") == 0) {
            int w = atoi(value);
            if (w >= 400 && w <= 4096) { g_state.window_width = w; g_have_saved_size = true; }
        } else if (strcmp(key, "window_height") == 0) {
            int h = atoi(value);
            if (h >= 300 && h <= 4096) { g_state.window_height = h; g_have_saved_size = true; }
        } else if (strcmp(key, "last_port") == 0) {
            strncpy(g_state.last_port_name, value, sizeof(g_state.last_port_name) - 1);
        } else if (strcmp(key, "last_save_directory") == 0) {
            strncpy(g_state.last_save_directory, value, sizeof(g_state.last_save_directory) - 1);
        } else if (strcmp(key, "baud_index") == 0) {
            int b = atoi(value);
            if (b >= 0 && b < BAUD_RATE_COUNT) g_state.baud_index = b;
        } else if (strcmp(key, "rotation") == 0) {
            int r = atoi(value);
            if (r == ROT_CW_90 || r == ROT_CCW_90) g_state.rotation = r;
        } else if (strcmp(key, "mirror") == 0) {
            g_state.mirror = (atoi(value) != 0);
        }
    }
    fclose(f);
}

static void SaveSettings() {
    char iniPath[PATH_MAX_LEN];
    GetSettingsPath(iniPath, sizeof(iniPath));

    FILE* f = fopen(iniPath, "w");
    if (!f) return;

    fprintf(f, "window_x=%d\n", g_state.window_x);
    fprintf(f, "window_y=%d\n", g_state.window_y);
    fprintf(f, "window_width=%d\n", g_state.window_width);
    fprintf(f, "window_height=%d\n", g_state.window_height);
    fprintf(f, "last_port=%s\n", g_state.last_port_name);
    fprintf(f, "last_save_directory=%s\n", g_state.last_save_directory);
    fprintf(f, "baud_index=%d\n", g_state.baud_index);
    fprintf(f, "rotation=%d\n", g_state.rotation);
    fprintf(f, "mirror=%d\n", g_state.mirror ? 1 : 0);
    fclose(f);
}

// =============================================================================
// Frame Processing (RGB565 -> RGBA)
// =============================================================================

// The frame arrives as 128 consecutive runs of 160 pixels; each run is one
// column of the display, so the raw image is the transpose of what the radio
// shows. Maps a display pixel back to its index in the raw frame.
inline int RawPixelIndex(int x, int y, int rotation, bool mirror) {
    int mx = mirror ? (DISPLAY_WIDTH - 1 - x) : x;
    int sx, sy;
    if (rotation == ROT_CW_90) {
        sx = y;
        sy = RAW_HEIGHT - 1 - mx;
    } else {
        sx = RAW_WIDTH - 1 - y;
        sy = mx;
    }
    return sy * RAW_WIDTH + sx;
}

void ProcessFrame(const uint8_t* raw, int rotation, bool mirror,
                  uint8_t* rgba_preview, uint8_t* rgba_thumb) {
    int preview_w = DISPLAY_WIDTH * PREVIEW_SCALE;

    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            // Decode one RGB565 little endian pixel
            int src = RawPixelIndex(x, y, rotation, mirror) * 2;
            int pixel = raw[src] | (raw[src + 1] << 8);

            uint8_t color[4];
            color[0] = (uint8_t)((((pixel >> 11) & 0x1F) * 255) / 31);  // R
            color[1] = (uint8_t)((((pixel >> 5) & 0x3F) * 255) / 63);   // G
            color[2] = (uint8_t)(((pixel & 0x1F) * 255) / 31);          // B
            color[3] = 0xFF;

            // Write to thumbnail (1x)
            memcpy(rgba_thumb + (y * DISPLAY_WIDTH + x) * 4, color, 4);

            // Write to preview (upscaled with nearest neighbor)
            for (int py = 0; py < PREVIEW_SCALE; py++) {
                for (int px = 0; px < PREVIEW_SCALE; px++) {
                    int preview_idx = ((y * PREVIEW_SCALE + py) * preview_w +
                                       (x * PREVIEW_SCALE + px)) * 4;
                    memcpy(rgba_preview + preview_idx, color, 4);
                }
            }
        }
    }
}

GLuint CreateTexture(const uint8_t* rgba, int width, int height) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

// Re-decode a screenshot from its raw frame, e.g. after the orientation changed
void RebuildScreenshot(Screenshot* ss) {
    ProcessFrame(ss->raw_frame, g_state.rotation, g_state.mirror,
                 ss->rgba_preview, ss->rgba_thumb);

    if (ss->texture_preview) glDeleteTextures(1, &ss->texture_preview);
    if (ss->texture_thumb) glDeleteTextures(1, &ss->texture_thumb);

    ss->texture_preview = CreateTexture(ss->rgba_preview,
                                        DISPLAY_WIDTH * PREVIEW_SCALE,
                                        DISPLAY_HEIGHT * PREVIEW_SCALE);
    ss->texture_thumb = CreateTexture(ss->rgba_thumb, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void RebuildAllScreenshots() {
    for (Screenshot* ss : g_state.screenshots) {
        RebuildScreenshot(ss);
    }
}

// =============================================================================
// Serial Port Functions
// =============================================================================

void EnumerateComPorts() {
    g_state.com_ports.clear();

#ifdef RADSHOT_WINDOWS
    static const GUID GUID_DEVINTERFACE_COMPORT =
        { 0x86E0D1E0L, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA devInfo = { sizeof(SP_DEVINFO_DATA) };

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); i++) {
        HKEY hKey = SetupDiOpenDevRegKey(
            hDevInfo, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ
        );

        if (hKey != INVALID_HANDLE_VALUE) {
            char portName[256];
            DWORD size = sizeof(portName);
            DWORD type;

            if (RegQueryValueExA(hKey, "PortName", nullptr, &type,
                                 (LPBYTE)portName, &size) == ERROR_SUCCESS) {
                if (strncmp(portName, "COM", 3) == 0) {
                    g_state.com_ports.push_back(portName);
                }
            }
            RegCloseKey(hKey);
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    std::sort(g_state.com_ports.begin(), g_state.com_ports.end(),
        [](const std::string& a, const std::string& b) {
            return atoi(a.c_str() + 3) < atoi(b.c_str() + 3);
        });

#elif defined(RADSHOT_MACOS)
    DIR* dir = opendir("/dev");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Match /dev/cu.* but exclude Bluetooth
        if (strncmp(entry->d_name, "cu.", 3) == 0 &&
            strstr(entry->d_name, "Bluetooth") == nullptr) {
            std::string path = std::string("/dev/") + entry->d_name;
            g_state.com_ports.push_back(path);
        }
    }
    closedir(dir);
    std::sort(g_state.com_ports.begin(), g_state.com_ports.end());

#else // Linux
    DIR* dir = opendir("/dev");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "ttyUSB", 6) == 0 ||
            strncmp(entry->d_name, "ttyACM", 6) == 0) {
            std::string path = std::string("/dev/") + entry->d_name;
            g_state.com_ports.push_back(path);
        }
    }
    closedir(dir);
    std::sort(g_state.com_ports.begin(), g_state.com_ports.end());
#endif
}

#ifndef RADSHOT_WINDOWS
// Maps a baud rate to its termios constant. Rates above 230400 have no constant
// on macOS - there they are set with IOSSIOSPEED after tcsetattr, which is what
// pyserial (and therefore at168-cps) does.
static bool BaudConstant(int baud, speed_t* out) {
    switch (baud) {
        case 115200: *out = B115200; return true;
        case 230400: *out = B230400; return true;
#ifdef B460800
        case 460800: *out = B460800; return true;
#endif
#ifdef B921600
        case 921600: *out = B921600; return true;
#endif
#ifdef B4000000
        case 4000000: *out = B4000000; return true;
#endif
        default: return false;
    }
}
#endif

static bool SerialDrainInput(double quiet_s = 0.05, double max_s = 0.5);

bool SerialConnect(const char* portName) {
    int baudrate = BAUD_RATES[g_state.baud_index];

#ifdef RADSHOT_WINDOWS
    char fullPath[256];
    snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", portName);

    g_state.serial_handle = CreateFileA(
        fullPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr
    );

    if (g_state.serial_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        const char* hint = "";
        if (err == ERROR_ACCESS_DENIED) hint = " (port busy - close any other CPS/terminal)";
        else if (err == ERROR_FILE_NOT_FOUND) hint = " (port gone - unplug/replug, then Refresh)";
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to open %s: error %lu%s", portName, (unsigned long)err, hint);
        return false;
    }

    DCB dcb = { sizeof(DCB) };
    if (!GetCommState(g_state.serial_handle, &dcb)) {
        DWORD err = GetLastError();
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to get port state: error %lu", (unsigned long)err);
        return false;
    }

    dcb.BaudRate = (DWORD)baudrate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    // dcb came from GetCommState, so it carries whatever the previous user of
    // the port left behind. Flow control still enabled from an earlier session
    // stalls every write waiting for CTS/DSR that the radio never asserts.
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(g_state.serial_handle, &dcb)) {
        DWORD err = GetLastError();
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to configure port: error %lu", (unsigned long)err);
        return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(g_state.serial_handle, &timeouts);

    PurgeComm(g_state.serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

#else // POSIX
    int fd = open(portName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to open %s: %s", portName, strerror(errno));
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to get port attributes: %s", strerror(errno));
        return false;
    }

    // The speed goes through cfset*speed, never OR-ed into c_cflag: the baud
    // constants are cflag bits on Linux but plain numbers on macOS, where
    // OR-ing one would corrupt the control flags.
    speed_t speed;
    bool standard_speed = BaudConstant(baudrate, &speed);
    if (!standard_speed) {
#ifdef RADSHOT_MACOS
        speed = B115200;  // placeholder, replaced by IOSSIOSPEED below
#else
        close(fd);
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Unsupported baud rate %d", baudrate);
        return false;
#endif
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // Clear only the framing bits we are about to set: on Linux the speed lives
    // in c_cflag too and cfset*speed just put it there. Hardware flow control
    // left on by whatever used the port last would stall us waiting for CTS.
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_iflag = IGNPAR;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to configure port: %s", strerror(errno));
        return false;
    }

#ifdef RADSHOT_MACOS
    if (!standard_speed) {
        speed_t requested = (speed_t)baudrate;
        if (ioctl(fd, IOSSIOSPEED, &requested) < 0) {
            close(fd);
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "Failed to set %d baud: %s", baudrate, strerror(errno));
            return false;
        }
    }
#endif

    g_state.serial_fd = fd;
#endif

    g_state.is_connected = true;

    // A previous session may have left the radio mid-transfer, so start from a
    // quiet line rather than inheriting its leftovers.
    SerialDrainInput();

    strncpy(g_state.last_port_name, portName, sizeof(g_state.last_port_name) - 1);
    snprintf(g_state.status_message, sizeof(g_state.status_message),
             "Connected to %s at %d baud", portName, baudrate);
    return true;
}

void SerialDisconnect() {
#ifdef RADSHOT_WINDOWS
    if (g_state.serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (g_state.serial_fd >= 0) {
        close(g_state.serial_fd);
        g_state.serial_fd = -1;
    }
#endif
    g_state.is_connected = false;
    g_state.is_capturing = false;
    strcpy(g_state.status_message, "Disconnected");
}

// Reads and discards whatever the radio is still sending, until the line has
// been quiet for quiet_s. A capture aborted part way leaves the tail of the old
// frame in flight, and flushing once at t=0 does not catch the bytes that
// arrive after it: the next capture then reads that leftover as the head of its
// frame, never sees a full one, and the app looks stuck until it times out.
// Bounded by max_s so this can never hang the UI thread.
// Returns true if the line actually went quiet. A radio that keeps talking for
// the whole budget - a Tier 3 set busy with trunked signalling, say - returns
// false, and whatever follows is not a clean capture.
static bool SerialDrainInput(double quiet_s, double max_s) {
    if (!g_state.is_connected) return true;

    bool went_quiet = false;

    uint8_t scratch[1024];
    double start = glfwGetTime();
    double last_data = start;

    for (;;) {
        double now = glfwGetTime();
        if (now - last_data >= quiet_s) { went_quiet = true; break; }
        if (now - start >= max_s) break;

#ifdef RADSHOT_WINDOWS
        DWORD n = 0;
        if (ReadFile(g_state.serial_handle, scratch, sizeof(scratch), &n, nullptr) && n > 0) {
            last_data = glfwGetTime();
            continue;
        }
        Sleep(2);
#else
        ssize_t n = read(g_state.serial_fd, scratch, sizeof(scratch));
        if (n > 0) {
            last_data = glfwGetTime();
            continue;
        }
        usleep(2000);
#endif
    }

    // The line is quiet now, so anything still buffered is stale.
#ifdef RADSHOT_WINDOWS
    PurgeComm(g_state.serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
    tcflush(g_state.serial_fd, TCIOFLUSH);
#endif

    return went_quiet;
}

void StartCapture() {
    if (!g_state.is_connected || g_state.is_capturing) return;

    // The sequence is drain -> SCR -> read -> drain, so the command always goes
    // out on a line with nothing stale left on it.
    //
    // Note this drains input only. There is deliberately no tcdrain/
    // FlushFileBuffers on the write: both block with no timeout, and a radio
    // that stopped accepting output would freeze the UI thread until the window
    // manager killed us. The command is three bytes and the kernel sends it
    // regardless.
    g_state.line_was_busy = !SerialDrainInput();

#ifdef RADSHOT_WINDOWS
    DWORD written = 0;
    if (!WriteFile(g_state.serial_handle, SCREENSHOT_CMD, sizeof(SCREENSHOT_CMD),
                   &written, nullptr) || written != sizeof(SCREENSHOT_CMD)) {
        strcpy(g_state.status_message, "Failed to send command - reconnect the radio");
        return;
    }
#else
    if (write(g_state.serial_fd, SCREENSHOT_CMD, sizeof(SCREENSHOT_CMD)) !=
        (ssize_t)sizeof(SCREENSHOT_CMD)) {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to send command: %s", strerror(errno));
        return;
    }
#endif

    g_state.is_capturing = true;
    g_state.capture_bytes = 0;
    g_state.capture_last_data = glfwGetTime();
    g_state.capture_progress = 0;
    memset(g_state.capture_buffer, 0, FRAME_SIZE);
}

void UpdateCapture() {
    if (!g_state.is_capturing) return;

    bool got_data = false;

    // Read straight into the frame buffer, asking for exactly what is still
    // missing, so a read can never return more than the frame has room for.
    // Going via a fixed scratch buffer meant a chunk crossing the end of the
    // frame had its tail consumed from the port and then dropped, leaving the
    // stream misaligned for whatever came next.
    for (int i = 0; i < READS_PER_FRAME && g_state.capture_bytes < FRAME_SIZE; i++) {
        int remaining = FRAME_SIZE - g_state.capture_bytes;
        uint8_t* dst = g_state.capture_buffer + g_state.capture_bytes;

#ifdef RADSHOT_WINDOWS
        DWORD bytesRead = 0;
        if (!ReadFile(g_state.serial_handle, dst, (DWORD)remaining, &bytesRead, nullptr) ||
            bytesRead == 0) {
            break;
        }
#else
        ssize_t bytesRead = read(g_state.serial_fd, dst, (size_t)remaining);
        // On POSIX, EAGAIN/EWOULDBLOCK means no data available (not an error)
        if (bytesRead <= 0) {
            break;
        }
#endif
        g_state.capture_bytes += (int)bytesRead;
        got_data = true;
    }

    if (got_data) {
        g_state.capture_progress = (g_state.capture_bytes * 100) / FRAME_SIZE;
        g_state.capture_last_data = glfwGetTime();

        if (g_state.capture_bytes >= FRAME_SIZE) {
            // Create new screenshot
            Screenshot* ss = new Screenshot();
            ss->id = g_state.next_id++;
            snprintf(ss->name, sizeof(ss->name), "screenshot_%03d", ss->id);

            time_t now = time(nullptr);
            struct tm* t = localtime(&now);
            ss->timestamp = { t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                              t->tm_hour, t->tm_min, t->tm_sec };

            ss->raw_frame = new uint8_t[FRAME_SIZE];
            memcpy(ss->raw_frame, g_state.capture_buffer, FRAME_SIZE);

            // Allocate and process
            int preview_w = DISPLAY_WIDTH * PREVIEW_SCALE;
            int preview_h = DISPLAY_HEIGHT * PREVIEW_SCALE;
            ss->rgba_preview = new uint8_t[preview_w * preview_h * 4];
            ss->rgba_thumb = new uint8_t[DISPLAY_WIDTH * DISPLAY_HEIGHT * 4];

            RebuildScreenshot(ss);

            g_state.screenshots.push_back(ss);
            g_state.selected_screenshot = (int)g_state.screenshots.size() - 1;
            strcpy(g_state.rename_buffer, ss->name);

            g_state.is_capturing = false;

            // Discard any trailing bytes so the next capture starts clean.
            SerialDrainInput();
        }
    } else if (glfwGetTime() - g_state.capture_last_data >= CAPTURE_QUIET_TIMEOUT_S) {
        if (g_state.capture_bytes == 0) {
            // Nothing at all came back: wrong baud rate and firmware
            // without the screenshot command look identical from here.
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "No response at %d baud - check the baud rate and that "
                     "the radio runs screenshot-enabled firmware",
                     BAUD_RATES[g_state.baud_index]);
        } else if (g_state.line_was_busy) {
            // No framing in the protocol, so anything the radio was already
            // sending is indistinguishable from the start of the frame.
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "Timeout: %d/%d bytes - radio was still sending when the "
                     "request went out, so the reply may be interleaved",
                     g_state.capture_bytes, FRAME_SIZE);
        } else {
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "Timeout: %d/%d bytes", g_state.capture_bytes, FRAME_SIZE);
        }
        g_state.is_capturing = false;

        // A partial frame leaves the radio mid-transfer; clear the leftover
        // so the next attempt is not decoding the tail of this one.
        SerialDrainInput();
    }
}

// =============================================================================
// File Operations
// =============================================================================

bool BrowseForFolder(char* path, size_t pathSize, const char* startDir) {
    const char* dir = (startDir && startDir[0]) ? startDir : nullptr;
    const char* result = tinyfd_selectFolderDialog("Select Folder", dir);
    if (result) {
        strncpy(path, result, pathSize - 1);
        path[pathSize - 1] = 0;
        return true;
    }
    return false;
}

bool SaveScreenshot(Screenshot* ss, const char* directory) {
    char filepath[PATH_MAX_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s.png", directory, ss->name);

    int w = DISPLAY_WIDTH * PREVIEW_SCALE;
    int h = DISPLAY_HEIGHT * PREVIEW_SCALE;
    return stbi_write_png(filepath, w, h, 4, ss->rgba_preview, w * 4) != 0;
}

void SaveSelected() {
    if (g_state.selected_screenshot < 0) return;

    char folder[PATH_MAX_LEN] = {0};
    if (BrowseForFolder(folder, sizeof(folder), g_state.last_save_directory)) {
        strncpy(g_state.last_save_directory, folder, sizeof(g_state.last_save_directory) - 1);
        Screenshot* ss = g_state.screenshots[g_state.selected_screenshot];
        if (SaveScreenshot(ss, folder)) {
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "Saved %s.png", ss->name);
        } else {
            strcpy(g_state.status_message, "Failed to save file");
        }
    }
}

void CopyToClipboard() {
    if (g_state.selected_screenshot < 0) return;

    Screenshot* ss = g_state.screenshots[g_state.selected_screenshot];
    int w = DISPLAY_WIDTH * PREVIEW_SCALE;
    int h = DISPLAY_HEIGHT * PREVIEW_SCALE;

#ifdef RADSHOT_WINDOWS
    // Calculate DIB size (BGR, 24-bit, DWORD-aligned rows)
    int rowBytes = ((w * 3 + 3) / 4) * 4;
    size_t dibSize = sizeof(BITMAPINFOHEADER) + rowBytes * h;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!hMem) {
        strcpy(g_state.status_message, "Failed to allocate clipboard memory");
        return;
    }

    uint8_t* pMem = (uint8_t*)GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        strcpy(g_state.status_message, "Failed to lock clipboard memory");
        return;
    }

    // Fill BITMAPINFOHEADER
    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pMem;
    memset(bih, 0, sizeof(BITMAPINFOHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;

    // Convert RGBA to BGR and flip vertically
    uint8_t* pixels = pMem + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        int srcY = h - 1 - y;
        for (int x = 0; x < w; x++) {
            int srcIdx = (srcY * w + x) * 4;
            int dstIdx = y * rowBytes + x * 3;
            pixels[dstIdx + 0] = ss->rgba_preview[srcIdx + 2];  // B
            pixels[dstIdx + 1] = ss->rgba_preview[srcIdx + 1];  // G
            pixels[dstIdx + 2] = ss->rgba_preview[srcIdx + 0];  // R
        }
    }

    GlobalUnlock(hMem);

    HWND hwnd = glfwGetWin32Window(g_state.window);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(hMem);
        strcpy(g_state.status_message, "Failed to open clipboard");
        return;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_DIB, hMem)) {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Copied %s to clipboard", ss->name);
    } else {
        GlobalFree(hMem);
        strcpy(g_state.status_message, "Failed to set clipboard data");
    }
    CloseClipboard();

#else
    // Write to temp PNG, then use platform tools to copy to clipboard
    char tmpPath[PATH_MAX_LEN];
    snprintf(tmpPath, sizeof(tmpPath), "/tmp/at168shot_clipboard.png");
    if (!stbi_write_png(tmpPath, w, h, 4, ss->rgba_preview, w * 4)) {
        strcpy(g_state.status_message, "Failed to write temp file for clipboard");
        return;
    }

    int ret = -1;
#ifdef RADSHOT_MACOS
    char cmd[PATH_MAX_LEN];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'set the clipboard to (read (POSIX file \"%s\") as «class PNGf»)'", tmpPath);
    ret = system(cmd);
#else
    // Try xclip first, then wl-copy
    char cmd[PATH_MAX_LEN];
    snprintf(cmd, sizeof(cmd),
        "xclip -selection clipboard -t image/png -i '%s' 2>/dev/null || "
        "wl-copy --type image/png < '%s' 2>/dev/null", tmpPath, tmpPath);
    ret = system(cmd);
#endif

    if (ret == 0) {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Copied %s to clipboard", ss->name);
    } else {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Clipboard copy failed (install xclip or wl-copy)");
    }

    remove(tmpPath);
#endif
}

void SaveAll() {
    if (g_state.screenshots.empty()) return;

    char folder[PATH_MAX_LEN] = {0};
    if (BrowseForFolder(folder, sizeof(folder), g_state.last_save_directory)) {
        strncpy(g_state.last_save_directory, folder, sizeof(g_state.last_save_directory) - 1);
        int saved = 0;
        for (Screenshot* ss : g_state.screenshots) {
            if (SaveScreenshot(ss, folder)) saved++;
        }
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Saved %d/%d screenshots", saved, (int)g_state.screenshots.size());
    }
}

void DeleteSelected() {
    if (g_state.selected_screenshot < 0) return;

    delete g_state.screenshots[g_state.selected_screenshot];
    g_state.screenshots.erase(g_state.screenshots.begin() + g_state.selected_screenshot);

    if (g_state.selected_screenshot >= (int)g_state.screenshots.size()) {
        g_state.selected_screenshot = (int)g_state.screenshots.size() - 1;
    }

    if (g_state.selected_screenshot >= 0) {
        strcpy(g_state.rename_buffer, g_state.screenshots[g_state.selected_screenshot]->name);
    } else {
        g_state.rename_buffer[0] = 0;
    }
}

void ClearAll() {
    for (Screenshot* ss : g_state.screenshots) {
        delete ss;
    }
    g_state.screenshots.clear();
    g_state.selected_screenshot = -1;
    g_state.rename_buffer[0] = 0;
}

// =============================================================================
// UI Rendering
// =============================================================================

void RenderUI() {
    ImGuiIO& io = ImGui::GetIO();

    // Full window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("AT168Shot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // === Connection Section ===
    if (ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Serial Port:");
        ImGui::SameLine();

        ImGui::SetNextItemWidth(200 * g_dpi_scale);
        const char* preview = g_state.selected_port >= 0 ?
            g_state.com_ports[g_state.selected_port].c_str() : "Select...";

        ImGui::BeginDisabled(g_state.is_connected);
        if (ImGui::BeginCombo("##port", preview)) {
            for (int i = 0; i < (int)g_state.com_ports.size(); i++) {
                bool selected = (i == g_state.selected_port);
                if (ImGui::Selectable(g_state.com_ports[i].c_str(), selected)) {
                    g_state.selected_port = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(g_state.is_connected);
        if (ImGui::Button("Refresh")) {
            EnumerateComPorts();
            g_state.selected_port = -1;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * g_dpi_scale);
        char baud_labels[BAUD_RATE_COUNT][16];
        const char* baud_items[BAUD_RATE_COUNT];
        for (int i = 0; i < BAUD_RATE_COUNT; i++) {
            snprintf(baud_labels[i], sizeof(baud_labels[i]), "%d", BAUD_RATES[i]);
            baud_items[i] = baud_labels[i];
        }
        ImGui::Combo("##baud", &g_state.baud_index, baud_items, BAUD_RATE_COUNT);
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (!g_state.is_connected) {
            ImGui::BeginDisabled(g_state.selected_port < 0);
            if (ImGui::Button("Connect")) {
                SerialConnect(g_state.com_ports[g_state.selected_port].c_str());
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Disconnect")) {
                SerialDisconnect();
            }
        }

        ImGui::SameLine();
        ImVec4 statusColor = g_state.is_connected ?
            ImVec4(0.0f, 0.8f, 0.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        ImGui::TextColored(statusColor, "%s", g_state.status_message);
    }

    // === Capture Section ===
    ImGui::Separator();
    ImGui::BeginDisabled(!g_state.is_connected || g_state.is_capturing);
    if (ImGui::Button("Take Screenshot", ImVec2(150 * g_dpi_scale, 30 * g_dpi_scale))) {
        StartCapture();
    }
    ImGui::EndDisabled();

    if (g_state.is_capturing) {
        ImGui::SameLine();
        ImGui::Text("Capturing... %d%%", g_state.capture_progress);
    }

    // === Gallery Section ===
    ImGui::Separator();
    ImGui::Text("Screenshots (%d captured)", (int)g_state.screenshots.size());

    ImGui::BeginChild("Gallery", ImVec2(0, 150 * g_dpi_scale), true,
        ImGuiWindowFlags_HorizontalScrollbar);

    float thumbW = (float)DISPLAY_WIDTH / THUMB_DIVISOR * g_dpi_scale;
    float thumbH = (float)DISPLAY_HEIGHT / THUMB_DIVISOR * g_dpi_scale;

    for (int i = 0; i < (int)g_state.screenshots.size(); i++) {
        if (i % GALLERY_COLUMNS != 0) ImGui::SameLine();

        Screenshot* ss = g_state.screenshots[i];
        ImGui::BeginGroup();

        bool selected = (i == g_state.selected_screenshot);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
        }

        ImGui::PushID(i);
        if (ImGui::ImageButton("##thumb", (ImTextureID)(intptr_t)ss->texture_thumb,
                               ImVec2(thumbW, thumbH))) {
            g_state.selected_screenshot = i;
            strcpy(g_state.rename_buffer, ss->name);
        }
        ImGui::PopID();

        if (selected) {
            ImGui::PopStyleColor(2);
        }

        // Truncate long names
        char displayName[20];
        if (strlen(ss->name) > 15) {
            strncpy(displayName, ss->name, 12);
            strcpy(displayName + 12, "...");
        } else {
            strcpy(displayName, ss->name);
        }

        float textW = ImGui::CalcTextSize(displayName).x;
        float offset = (thumbW - textW) * 0.5f;
        if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        ImGui::TextUnformatted(displayName);

        ImGui::EndGroup();
    }

    ImGui::EndChild();

    // === Selected Screenshot Section ===
    ImGui::Separator();
    if (g_state.selected_screenshot >= 0) {
        Screenshot* ss = g_state.screenshots[g_state.selected_screenshot];
        ImGui::Text("Selected: %s", ss->name);

        ImGui::SetNextItemWidth(200 * g_dpi_scale);
        ImGui::InputText("##rename", g_state.rename_buffer, sizeof(g_state.rename_buffer));

        ImGui::SameLine();
        if (ImGui::Button("Rename")) {
            if (strlen(g_state.rename_buffer) > 0) {
                strcpy(ss->name, g_state.rename_buffer);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            g_state.show_delete_popup = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            SaveSelected();
        }

        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            CopyToClipboard();
        }
    } else {
        ImGui::TextDisabled("No screenshot selected");
    }

    // === Preview Section ===
    ImGui::Separator();
    ImGui::Text("Preview");
    ImGui::SameLine();

    // The radio streams the frame transposed; if it comes out sideways or
    // mirrored, these two settings cover every possible orientation.
    ImGui::SetNextItemWidth(130 * g_dpi_scale);
    const char* rotations[] = { "Rotate 90 CW", "Rotate 90 CCW" };
    if (ImGui::Combo("##rotation", &g_state.rotation, rotations, 2)) {
        RebuildAllScreenshots();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Mirror", &g_state.mirror)) {
        RebuildAllScreenshots();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Adjust until the capture matches the radio display.\n"
                               "The setting is remembered between runs.");
        ImGui::EndTooltip();
    }

    if (g_state.selected_screenshot >= 0) {
        Screenshot* ss = g_state.screenshots[g_state.selected_screenshot];
        float previewW = DISPLAY_WIDTH * PREVIEW_SCALE * g_dpi_scale;
        float previewH = DISPLAY_HEIGHT * PREVIEW_SCALE * g_dpi_scale;
        ImGui::Image((ImTextureID)(intptr_t)ss->texture_preview,
                     ImVec2(previewW, previewH));
    } else {
        ImGui::TextDisabled("Take a screenshot to see preview");
    }

    // === Bottom Buttons ===
    ImGui::Separator();
    ImGui::BeginDisabled(g_state.screenshots.empty());
    if (ImGui::Button("Save All")) {
        SaveAll();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        g_state.show_clear_popup = true;
    }
    ImGui::EndDisabled();

    ImGui::End();

    // === Confirmation Popups ===
    if (g_state.show_delete_popup) {
        ImGui::OpenPopup("Delete?");
        g_state.show_delete_popup = false;
    }

    if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'?", g_state.screenshots[g_state.selected_screenshot]->name);
        ImGui::Separator();

        if (ImGui::Button("Yes", ImVec2(80 * g_dpi_scale, 0))) {
            DeleteSelected();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80 * g_dpi_scale, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_state.show_clear_popup) {
        ImGui::OpenPopup("Clear All?");
        g_state.show_clear_popup = false;
    }

    if (ImGui::BeginPopupModal("Clear All?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Clear all screenshots?\nUnsaved screenshots will be lost.");
        ImGui::Separator();

        if (ImGui::Button("Yes", ImVec2(80 * g_dpi_scale, 0))) {
            ClearAll();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80 * g_dpi_scale, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_state.show_exit_popup) {
        ImGui::OpenPopup("Exit?");
        g_state.show_exit_popup = false;
    }

    if (ImGui::BeginPopupModal("Exit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved screenshots.\nExit anyway?");
        ImGui::Separator();

        if (ImGui::Button("Yes", ImVec2(80 * g_dpi_scale, 0))) {
            g_state.running = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80 * g_dpi_scale, 0))) {
            g_state.pending_close = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// =============================================================================
// GLFW Callbacks
// =============================================================================

static void glfw_window_close_callback(GLFWwindow* window) {
    if (!g_state.screenshots.empty()) {
        g_state.show_exit_popup = true;
        g_state.pending_close = true;
        glfwSetWindowShouldClose(window, GLFW_FALSE);
    } else {
        g_state.running = false;
    }
}

// =============================================================================
// Entry Point
// =============================================================================

#ifdef RADSHOT_WINDOWS
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main(int, char**) {
#endif
    // Load saved settings
    LoadSettings();

    // Initialize GLFW
    if (!glfwInit()) {
        return 1;
    }

    // GL 3.0 + GLSL 130
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // Create window
    char title[256];
    snprintf(title, sizeof(title), "AT168Shot v%s - AnyTone 168 Screenshot Tool", APP_VERSION);

    // The default size is authored for 96 DPI; grow it to match the monitor.
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    if (!g_have_saved_size && primary) {
        float mscale = ImGui_ImplGlfw_GetContentScaleForMonitor(primary);
        if (mscale > 0.0f) {
            g_state.window_width = (int)(g_state.window_width * mscale);
            g_state.window_height = (int)(g_state.window_height * mscale);
        }
    }

    GLFWwindow* window = glfwCreateWindow(
        g_state.window_width, g_state.window_height, title, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    g_state.window = window;

    // Restore window position
    if (g_state.window_x >= 0 && g_state.window_y >= 0) {
        glfwSetWindowPos(window, g_state.window_x, g_state.window_y);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync
    glfwSetWindowCloseCallback(window, glfw_window_close_callback);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Disable imgui.ini

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();

    g_base_style = ImGui::GetStyle();
    ApplyDpiScale(QueryDpiScale(window));

    // Initial port enumeration
    EnumerateComPorts();

    // Restore saved port selection
    if (g_state.last_port_name[0] != 0) {
        for (int i = 0; i < (int)g_state.com_ports.size(); i++) {
            if (g_state.com_ports[i] == g_state.last_port_name) {
                g_state.selected_port = i;
                break;
            }
        }
    }

    // Main loop
    while (g_state.running && !glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Track window position and size
        glfwGetWindowPos(window, &g_state.window_x, &g_state.window_y);
        glfwGetWindowSize(window, &g_state.window_width, &g_state.window_height);

        // Follow DPI changes, e.g. dragging onto a monitor with a different scale
        float scale = QueryDpiScale(window);
        if (fabsf(scale - g_dpi_scale) > 0.001f) {
            ApplyDpiScale(scale);
        }

        // Update capture
        UpdateCapture();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render UI
        RenderUI();

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Save settings before exiting
    SaveSettings();

    // Cleanup
    ClearAll();
    SerialDisconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
