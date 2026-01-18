// RadShot - Screenshot capture tool for Radtel RT-4D radio displays
// C++ Win32 + Dear ImGui reimplementation for minimal binary size

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <GL/gl.h>

#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_opengl3.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =============================================================================
// Constants
// =============================================================================

constexpr const char* APP_VERSION = "0.1";

constexpr int DISPLAY_WIDTH = 128;
constexpr int DISPLAY_HEIGHT = 64;
constexpr int BITMAP_SIZE = 1024;
constexpr DWORD BAUDRATE = 115200;
constexpr uint8_t SCREENSHOT_CMD[] = { 0x41, 0x41 };
constexpr int PREVIEW_SCALE = 4;
constexpr int GALLERY_COLUMNS = 4;
constexpr int MAX_RETRIES = 100;

// Color palette from original (BGR format in BMP, but we use RGBA here)
constexpr uint8_t COLOR_LIGHT[4] = { 0xDE, 0xEB, 0xFF, 0xFF }; // Light blue tint
constexpr uint8_t COLOR_DARK[4] = { 0x00, 0x00, 0x00, 0xFF };  // Black

// GUID for COM ports
static const GUID GUID_DEVINTERFACE_COMPORT =
    { 0x86E0D1E0L, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

// =============================================================================
// Screenshot Structure
// =============================================================================

struct Screenshot {
    int id;
    char name[256];
    uint8_t raw_bitmap[BITMAP_SIZE];
    uint8_t* rgba_preview;  // DISPLAY_WIDTH*PREVIEW_SCALE x DISPLAY_HEIGHT*PREVIEW_SCALE x 4
    uint8_t* rgba_thumb;    // DISPLAY_WIDTH x DISPLAY_HEIGHT x 4
    GLuint texture_preview;
    GLuint texture_thumb;
    SYSTEMTIME timestamp;

    Screenshot() : id(0), rgba_preview(nullptr), rgba_thumb(nullptr),
                   texture_preview(0), texture_thumb(0) {
        name[0] = 0;
        memset(raw_bitmap, 0, BITMAP_SIZE);
        memset(&timestamp, 0, sizeof(timestamp));
    }

    ~Screenshot() {
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
        memcpy(raw_bitmap, other.raw_bitmap, BITMAP_SIZE);
        rgba_preview = other.rgba_preview;
        rgba_thumb = other.rgba_thumb;
        texture_preview = other.texture_preview;
        texture_thumb = other.texture_thumb;
        timestamp = other.timestamp;
        other.rgba_preview = nullptr;
        other.rgba_thumb = nullptr;
        other.texture_preview = 0;
        other.texture_thumb = 0;
    }

    Screenshot& operator=(Screenshot&& other) noexcept {
        if (this != &other) {
            if (rgba_preview) delete[] rgba_preview;
            if (rgba_thumb) delete[] rgba_thumb;
            if (texture_preview) glDeleteTextures(1, &texture_preview);
            if (texture_thumb) glDeleteTextures(1, &texture_thumb);

            id = other.id;
            strcpy(name, other.name);
            memcpy(raw_bitmap, other.raw_bitmap, BITMAP_SIZE);
            rgba_preview = other.rgba_preview;
            rgba_thumb = other.rgba_thumb;
            texture_preview = other.texture_preview;
            texture_thumb = other.texture_thumb;
            timestamp = other.timestamp;
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
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;
    int window_width = 800;
    int window_height = 700;
    bool running = true;

    // Serial
    std::vector<std::string> com_ports;
    int selected_port = -1;
    HANDLE serial_handle = INVALID_HANDLE_VALUE;
    bool is_connected = false;
    char status_message[256] = "Disconnected";

    // Capture
    bool is_capturing = false;
    int capture_progress = 0;
    uint8_t capture_buffer[BITMAP_SIZE];
    int capture_bytes = 0;
    int capture_retries = 0;

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
    char last_save_directory[MAX_PATH] = {0};
    char last_port_name[32] = {0};
    int window_x = CW_USEDEFAULT;
    int window_y = CW_USEDEFAULT;
};

static AppState g_state;

// =============================================================================
// Settings Persistence
// =============================================================================

static void GetSettingsPath(char* path, size_t pathSize) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    // Replace .exe with .ini
    char* dot = strrchr(exePath, '.');
    if (dot) {
        strcpy(dot, ".ini");
    } else {
        strcat(exePath, ".ini");
    }
    strncpy(path, exePath, pathSize - 1);
    path[pathSize - 1] = 0;
}

static void LoadSettings() {
    char iniPath[MAX_PATH];
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
            if (w >= 400 && w <= 4096) g_state.window_width = w;
        } else if (strcmp(key, "window_height") == 0) {
            int h = atoi(value);
            if (h >= 300 && h <= 4096) g_state.window_height = h;
        } else if (strcmp(key, "last_port") == 0) {
            strncpy(g_state.last_port_name, value, sizeof(g_state.last_port_name) - 1);
        } else if (strcmp(key, "last_save_directory") == 0) {
            strncpy(g_state.last_save_directory, value, sizeof(g_state.last_save_directory) - 1);
        }
    }
    fclose(f);
}

static void SaveSettings() {
    char iniPath[MAX_PATH];
    GetSettingsPath(iniPath, sizeof(iniPath));

    FILE* f = fopen(iniPath, "w");
    if (!f) return;

    fprintf(f, "window_x=%d\n", g_state.window_x);
    fprintf(f, "window_y=%d\n", g_state.window_y);
    fprintf(f, "window_width=%d\n", g_state.window_width);
    fprintf(f, "window_height=%d\n", g_state.window_height);
    fprintf(f, "last_port=%s\n", g_state.last_port_name);
    fprintf(f, "last_save_directory=%s\n", g_state.last_save_directory);
    fclose(f);
}

// =============================================================================
// Bitmap Processing (matching Python implementation)
// =============================================================================

inline int GetPixel(const uint8_t* bitmap, int x, int y) {
    return (bitmap[x + ((y / 8) * DISPLAY_WIDTH)] >> (y & 7)) & 1;
}

inline void SetPixel(uint8_t* buffer, int x, int y, int color) {
    buffer[(x / 8) + ((DISPLAY_HEIGHT - 1 - y) * 16)] |= (color << (7 - (x & 7)));
}

void ProcessBitmap(const uint8_t* raw, uint8_t* rgba_preview, uint8_t* rgba_thumb) {
    // Convert raw device format to standard format
    uint8_t processed[BITMAP_SIZE] = {0};
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            SetPixel(processed, x, y, GetPixel(raw, x, y));
        }
    }

    // Generate preview (4x upscaled) and thumbnail
    int preview_w = DISPLAY_WIDTH * PREVIEW_SCALE;
    int preview_h = DISPLAY_HEIGHT * PREVIEW_SCALE;

    for (int sy = 0; sy < DISPLAY_HEIGHT; sy++) {
        for (int sx = 0; sx < DISPLAY_WIDTH; sx++) {
            // Read from processed bitmap
            int byte_idx = (sx / 8) + ((DISPLAY_HEIGHT - 1 - sy) * 16);
            int bit_idx = 7 - (sx & 7);
            int pixel = (processed[byte_idx] >> bit_idx) & 1;

            const uint8_t* color = pixel ? COLOR_DARK : COLOR_LIGHT;

            // Write to thumbnail (1x)
            int thumb_idx = (sy * DISPLAY_WIDTH + sx) * 4;
            memcpy(rgba_thumb + thumb_idx, color, 4);

            // Write to preview (4x upscaled with nearest neighbor)
            for (int py = 0; py < PREVIEW_SCALE; py++) {
                for (int px = 0; px < PREVIEW_SCALE; px++) {
                    int preview_x = sx * PREVIEW_SCALE + px;
                    int preview_y = sy * PREVIEW_SCALE + py;
                    int preview_idx = (preview_y * preview_w + preview_x) * 4;
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

// =============================================================================
// Serial Port Functions
// =============================================================================

void EnumerateComPorts() {
    g_state.com_ports.clear();

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

    // Natural sort
    std::sort(g_state.com_ports.begin(), g_state.com_ports.end(),
        [](const std::string& a, const std::string& b) {
            return atoi(a.c_str() + 3) < atoi(b.c_str() + 3);
        });
}

bool SerialConnect(const char* portName) {
    char fullPath[32];
    snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", portName);

    g_state.serial_handle = CreateFileA(
        fullPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr
    );

    if (g_state.serial_handle == INVALID_HANDLE_VALUE) {
        snprintf(g_state.status_message, sizeof(g_state.status_message),
                 "Failed to open %s", portName);
        return false;
    }

    DCB dcb = { sizeof(DCB) };
    if (!GetCommState(g_state.serial_handle, &dcb)) {
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        strcpy(g_state.status_message, "Failed to get port state");
        return false;
    }

    dcb.BaudRate = BAUDRATE;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(g_state.serial_handle, &dcb)) {
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        strcpy(g_state.status_message, "Failed to configure port");
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

    g_state.is_connected = true;
    strncpy(g_state.last_port_name, portName, sizeof(g_state.last_port_name) - 1);
    snprintf(g_state.status_message, sizeof(g_state.status_message),
             "Connected to %s", portName);
    return true;
}

void SerialDisconnect() {
    if (g_state.serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
    }
    g_state.is_connected = false;
    g_state.is_capturing = false;
    strcpy(g_state.status_message, "Disconnected");
}

void StartCapture() {
    if (!g_state.is_connected || g_state.is_capturing) return;

    PurgeComm(g_state.serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    DWORD written;
    WriteFile(g_state.serial_handle, SCREENSHOT_CMD, sizeof(SCREENSHOT_CMD), &written, nullptr);
    FlushFileBuffers(g_state.serial_handle);

    g_state.is_capturing = true;
    g_state.capture_bytes = 0;
    g_state.capture_retries = 0;
    g_state.capture_progress = 0;
    memset(g_state.capture_buffer, 0, BITMAP_SIZE);
}

void UpdateCapture() {
    if (!g_state.is_capturing) return;

    uint8_t temp[256];
    DWORD bytesRead;

    if (ReadFile(g_state.serial_handle, temp, sizeof(temp), &bytesRead, nullptr) && bytesRead > 0) {
        int toCopy = (std::min)((int)bytesRead, BITMAP_SIZE - g_state.capture_bytes);
        memcpy(g_state.capture_buffer + g_state.capture_bytes, temp, toCopy);
        g_state.capture_bytes += toCopy;
        g_state.capture_progress = (g_state.capture_bytes * 100) / BITMAP_SIZE;
        g_state.capture_retries = 0;

        if (g_state.capture_bytes >= BITMAP_SIZE) {
            // Create new screenshot
            Screenshot* ss = new Screenshot();
            ss->id = g_state.next_id++;
            snprintf(ss->name, sizeof(ss->name), "screenshot_%03d", ss->id);
            GetLocalTime(&ss->timestamp);
            memcpy(ss->raw_bitmap, g_state.capture_buffer, BITMAP_SIZE);

            // Allocate and process
            int preview_w = DISPLAY_WIDTH * PREVIEW_SCALE;
            int preview_h = DISPLAY_HEIGHT * PREVIEW_SCALE;
            ss->rgba_preview = new uint8_t[preview_w * preview_h * 4];
            ss->rgba_thumb = new uint8_t[DISPLAY_WIDTH * DISPLAY_HEIGHT * 4];

            ProcessBitmap(ss->raw_bitmap, ss->rgba_preview, ss->rgba_thumb);

            ss->texture_preview = CreateTexture(ss->rgba_preview, preview_w, preview_h);
            ss->texture_thumb = CreateTexture(ss->rgba_thumb, DISPLAY_WIDTH, DISPLAY_HEIGHT);

            g_state.screenshots.push_back(ss);
            g_state.selected_screenshot = (int)g_state.screenshots.size() - 1;
            strcpy(g_state.rename_buffer, ss->name);

            g_state.is_capturing = false;
        }
    } else {
        g_state.capture_retries++;
        if (g_state.capture_retries >= MAX_RETRIES) {
            snprintf(g_state.status_message, sizeof(g_state.status_message),
                     "Timeout: %d/%d bytes", g_state.capture_bytes, BITMAP_SIZE);
            g_state.is_capturing = false;
        }
    }
}

// =============================================================================
// File Operations
// =============================================================================

bool BrowseForFolder(char* path, size_t pathSize, const char* startDir) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileDialog* pfd = nullptr;
    bool result = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD options;
        pfd->GetOptions(&options);
        pfd->SetOptions(options | FOS_PICKFOLDERS);

        // Set initial folder if provided
        if (startDir && startDir[0] != 0) {
            wchar_t startDirW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, startDir, -1, startDirW, MAX_PATH);
            IShellItem* psiFolder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(startDirW, nullptr, IID_PPV_ARGS(&psiFolder)))) {
                pfd->SetFolder(psiFolder);
                psiFolder->Release();
            }
        }

        if (SUCCEEDED(pfd->Show(g_state.hwnd))) {
            IShellItem* psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, path, (int)pathSize, nullptr, nullptr);
                    CoTaskMemFree(pszPath);
                    result = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }

    CoUninitialize();
    return result;
}

bool SaveScreenshot(Screenshot* ss, const char* directory) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s\\%s.png", directory, ss->name);

    int w = DISPLAY_WIDTH * PREVIEW_SCALE;
    int h = DISPLAY_HEIGHT * PREVIEW_SCALE;
    return stbi_write_png(filepath, w, h, 4, ss->rgba_preview, w * 4) != 0;
}

void SaveSelected() {
    if (g_state.selected_screenshot < 0) return;

    char folder[MAX_PATH] = {0};
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
    bih->biHeight = h;  // Positive = bottom-up DIB
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;

    // Convert RGBA to BGR and flip vertically
    uint8_t* pixels = pMem + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        int srcY = h - 1 - y;  // Flip: DIB is bottom-up
        for (int x = 0; x < w; x++) {
            int srcIdx = (srcY * w + x) * 4;
            int dstIdx = y * rowBytes + x * 3;
            pixels[dstIdx + 0] = ss->rgba_preview[srcIdx + 2];  // B
            pixels[dstIdx + 1] = ss->rgba_preview[srcIdx + 1];  // G
            pixels[dstIdx + 2] = ss->rgba_preview[srcIdx + 0];  // R
        }
    }

    GlobalUnlock(hMem);

    if (!OpenClipboard(g_state.hwnd)) {
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
}

void SaveAll() {
    if (g_state.screenshots.empty()) return;

    char folder[MAX_PATH] = {0};
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
    ImGui::Begin("RadShot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // === Connection Section ===
    if (ImGui::CollapsingHeader("Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Serial Port:");
        ImGui::SameLine();

        ImGui::SetNextItemWidth(120);
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
    if (ImGui::Button("Take Screenshot", ImVec2(150, 30))) {
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

    ImGui::BeginChild("Gallery", ImVec2(0, 180), true,
        ImGuiWindowFlags_HorizontalScrollbar);

    float thumbW = (float)DISPLAY_WIDTH;
    float thumbH = (float)DISPLAY_HEIGHT;

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

        ImGui::SetNextItemWidth(200);
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

    if (g_state.selected_screenshot >= 0) {
        Screenshot* ss = g_state.screenshots[g_state.selected_screenshot];
        int previewW = DISPLAY_WIDTH * PREVIEW_SCALE;
        int previewH = DISPLAY_HEIGHT * PREVIEW_SCALE;
        ImGui::Image((ImTextureID)(intptr_t)ss->texture_preview,
                     ImVec2((float)previewW, (float)previewH));
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

        if (ImGui::Button("Yes", ImVec2(80, 0))) {
            DeleteSelected();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80, 0))) {
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

        if (ImGui::Button("Yes", ImVec2(80, 0))) {
            ClearAll();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80, 0))) {
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

        if (ImGui::Button("Yes", ImVec2(80, 0))) {
            g_state.running = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80, 0))) {
            g_state.pending_close = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// =============================================================================
// OpenGL Context (WGL)
// =============================================================================

bool CreateGLContext() {
    g_state.hdc = GetDC(g_state.hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int pf = ChoosePixelFormat(g_state.hdc, &pfd);
    if (!pf) return false;

    if (!SetPixelFormat(g_state.hdc, pf, &pfd)) return false;

    g_state.hglrc = wglCreateContext(g_state.hdc);
    if (!g_state.hglrc) return false;

    if (!wglMakeCurrent(g_state.hdc, g_state.hglrc)) return false;

    return true;
}

void CleanupGLContext() {
    if (g_state.hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_state.hglrc);
    }
    if (g_state.hdc) {
        ReleaseDC(g_state.hwnd, g_state.hdc);
    }
}

// =============================================================================
// Window Procedure
// =============================================================================

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_state.window_width = LOWORD(lParam);
            g_state.window_height = HIWORD(lParam);
        }
        return 0;

    case WM_MOVE:
        g_state.window_x = (int)(short)LOWORD(lParam);
        g_state.window_y = (int)(short)HIWORD(lParam);
        return 0;

    case WM_CLOSE:
        if (!g_state.screenshots.empty()) {
            g_state.show_exit_popup = true;
            g_state.pending_close = true;
            return 0;
        }
        g_state.running = false;
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// =============================================================================
// Entry Point
// =============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Load saved settings
    LoadSettings();

    // Load application icon from resources (ID 1 defined in radshot.rc)
    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    HICON hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), 0);

    // Register window class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RadShotClass";
    RegisterClassExW(&wc);

    // Create window with version in title
    char titleA[256];
    snprintf(titleA, sizeof(titleA), "RadShot v%s - Radtel RT-4D Screenshot Tool", APP_VERSION);
    wchar_t titleW[256];
    MultiByteToWideChar(CP_UTF8, 0, titleA, -1, titleW, 256);

    g_state.hwnd = CreateWindowExW(
        0, L"RadShotClass", titleW,
        WS_OVERLAPPEDWINDOW,
        g_state.window_x, g_state.window_y, g_state.window_width, g_state.window_height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_state.hwnd) return 1;

    // Create OpenGL context
    if (!CreateGLContext()) {
        MessageBoxW(g_state.hwnd, L"Failed to create OpenGL context", L"Error", MB_OK);
        return 1;
    }

    ShowWindow(g_state.hwnd, nCmdShow);
    UpdateWindow(g_state.hwnd);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Disable imgui.ini

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_state.hwnd);
    ImGui_ImplOpenGL3_Init();

    // Initial port enumeration
    EnumerateComPorts();

    // Restore saved COM port selection
    if (g_state.last_port_name[0] != 0) {
        for (int i = 0; i < (int)g_state.com_ports.size(); i++) {
            if (g_state.com_ports[i] == g_state.last_port_name) {
                g_state.selected_port = i;
                break;
            }
        }
    }

    // Main loop
    while (g_state.running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_state.running = false;
            }
        }

        if (!g_state.running) break;

        // Update capture
        UpdateCapture();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render UI
        RenderUI();

        // Render
        ImGui::Render();
        glViewport(0, 0, g_state.window_width, g_state.window_height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_state.hdc);
    }

    // Save settings before exiting
    SaveSettings();

    // Cleanup
    ClearAll();
    SerialDisconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupGLContext();
    DestroyWindow(g_state.hwnd);
    UnregisterClassW(L"RadShotClass", hInstance);

    return 0;
}
