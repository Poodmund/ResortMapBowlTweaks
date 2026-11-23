// Ski-E-O Resort Map Bowl Tweaks v1.0.0
// Customises bowl centreline strokes and name labels in the Resort Map view.
// SPDX-License-Identifier: 0BSD

#include <Windows.h>
#include <MinHook.h>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <initializer_list>
#include <atomic>
#include <cctype>
#include <limits>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <cwctype>

namespace {

// Native function addresses are resolved by scanning executable code at runtime.
// The scan never interprets a non-unique or missing pattern as a valid address.
struct ResolvedNativeFunctions {
    std::uintptr_t generator = 0;
    std::uintptr_t paintLine = 0;
    std::uintptr_t installMapStrokes = 0;
    std::uintptr_t fullMapRebuild = 0;
    std::uintptr_t addMapLabel = 0;
};
ResolvedNativeFunctions g_nativeFunctions{};

constexpr int kMaxSourceRecords = 512;
constexpr std::size_t kSourceRecordBytes = 120;
constexpr std::size_t kSourceContextBytes = 192; // read by generator through +176
constexpr int kMaxCenterlineBowlPoints = 512;
constexpr int kCenterlineSlices = 32;

std::atomic<unsigned int> g_mapCalls{0};

// Dashed strokes use the game's native map-line painter.
// Default: 7 Slate-local units of ink, followed by 15 units of space.
enum class StrokeStyle { Solid, Dashed, Hidden };
enum class TextStyle { Default, Inverted, WhitePlate };
TextStyle g_textStyle = TextStyle::Default;
// These FLinearColor values correspond to gamma-corrected sRGB values.
// The user-facing INI uses ordinary #RRGGBB hex RGB, NOT linear floats.
std::array<float, 4> g_lightColor{0.863157f, 0.799103f, 0.658375f, 1.f}; // #EFE7D4
std::array<float, 4> g_darkColor{0.019382f, 0.019382f, 0.024158f, 1.f}; // #26262B
std::string g_lightHex = "#EFE7D4";
std::string g_darkHex = "#26262B";
std::filesystem::path g_modDirectory;
std::atomic<bool> g_debugEnabled{false}; // Never write logs unless INI debug=true.
// Use native geometry IDs for bowl labels by default. Name matching is
// available as an alternate mode when id_label_style=false.
bool g_idLabelStyleEnabled = true;
std::atomic<unsigned int> g_idLabelStylePasses{0};
std::atomic<unsigned int> g_labelPasses{0};
std::atomic<unsigned int> g_hiddenStrokeCalls{0};
std::mutex g_plateMutex;
std::unordered_map<std::uintptr_t, std::uintptr_t> g_platedTextBlocks; // TextBlock -> the inner UBorder
std::atomic<std::uintptr_t> g_plateCanvas{0};
std::atomic<std::uintptr_t> g_activeLabelCanvas{0}; // Map label panel for pre-attachment hook.
// Capture only during a native full-map rebuild, and release immediately
// after the ID styling pass. These indexes come from game-supplied UWidgets;
// native placement handles are never dereferenced as pointers.
std::atomic<bool> g_captureMapAttachments{false};
std::mutex g_mapAttachmentMutex;
std::unordered_map<std::uint32_t, std::uintptr_t> g_mapAttachmentByObjectIndex;
std::atomic<unsigned int> g_preparedLabelCount{0};

// Bowl names are read only when name-based identification is selected.
// Native ID mode does not require name scanning or text-based identification.
std::mutex g_bowlNamesMutex;
std::unordered_set<std::wstring> g_bowlNames;
std::atomic<unsigned int> g_bowlNamesUpdates{0};
// Defer hiding installed strokes until native label positioning is complete.
// The full-map rebuild hook also styles labels that are generated late.
using FullMapRebuildFn = double(__fastcall*)(std::uintptr_t,
                                             std::uintptr_t,
                                             std::uintptr_t);
FullMapRebuildFn g_originalFullMapRebuild = nullptr;
void* g_fullMapRebuildAddress = nullptr;
// The map-label attachment helper captures live widget identities for
// ID matching, or applies preliminary text styling in name-matching mode.
using AddMapLabelFn = std::uintptr_t(__fastcall*)(std::uintptr_t panel, std::uintptr_t child);
AddMapLabelFn g_originalAddMapLabel = nullptr;
void* g_addMapLabelAddress = nullptr;
using InstallMapStrokesFn = std::uintptr_t(__fastcall*)(std::uintptr_t mapWidget,
                                                        std::uintptr_t generatedStrokes);
InstallMapStrokesFn g_originalInstallMapStrokes = nullptr;
void* g_installMapStrokesAddress = nullptr;
RC::Unreal::UClass* g_textBlockClass = nullptr;
RC::Unreal::UClass* g_panelWidgetClass = nullptr;
RC::Unreal::UClass* g_userWidgetClass = nullptr;
RC::Unreal::UClass* g_richTextBlockClass = nullptr;
RC::Unreal::UClass* g_canvasPanelClass = nullptr;
RC::Unreal::UClass* g_canvasSlotClass = nullptr;
RC::Unreal::UClass* g_borderClass = nullptr;
RC::Unreal::UClass* g_contentWidgetClass = nullptr;
RC::Unreal::UFunction* g_setTextFn = nullptr;
RC::Unreal::UFunction* g_getParentFn = nullptr;
RC::Unreal::UFunction* g_setVisibilityFn = nullptr;
RC::Unreal::UFunction* g_canvasAddChildFn = nullptr;
RC::Unreal::UFunction* g_borderSetContentFn = nullptr;
RC::Unreal::UFunction* g_borderSetBrushFn = nullptr;
RC::Unreal::UFunction* g_borderSetPaddingFn = nullptr;
RC::Unreal::UFunction* g_contentSetContentFn = nullptr;
RC::Unreal::UFunction* g_panelRemoveChildFn = nullptr;
RC::Unreal::UFunction* g_canvasSlotSetLayoutFn = nullptr;
RC::Unreal::UFunction* g_canvasSlotSetAutoSizeFn = nullptr;
RC::Unreal::UFunction* g_canvasSlotSetZOrderFn = nullptr;
RC::Unreal::UFunction* g_setFontFn = nullptr;
RC::Unreal::UFunction* g_setColorFn = nullptr;
RC::Unreal::UFunction* g_getChildrenCountFn = nullptr;
RC::Unreal::UFunction* g_getChildAtFn = nullptr;
StrokeStyle g_strokeStyle = StrokeStyle::Dashed;
double g_dashLength = 7.0;
double g_gapLength = 15.0;
constexpr int kMaximumDashesPerStroke = 256;
std::array<std::atomic<std::uint64_t>, 8> g_bowlIndexBits{};
std::atomic<std::uintptr_t> g_mapStrokeLayer{0};
std::atomic<unsigned int> g_dashedStrokes{0};

// Parameters 1-5 are integer/pointer ABI slots. Parameters 6-7 are floats
// and live in stack slots under the Win64 calling convention. The original
// line painter is called via the MinHook trampoline with the same arguments.
using PaintLineFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t,
                                       std::uintptr_t, std::uintptr_t,
                                       std::uintptr_t, float, float, unsigned char);
PaintLineFn g_originalPaintLine = nullptr;
void* g_paintLineAddress = nullptr;

struct Vector2D { double x, y; };
static_assert(sizeof(Vector2D) == 16);

void publish_bowl_indices(const std::array<std::uint64_t, 8>& bits) {
    for (std::size_t i = 0; i < bits.size(); ++i) {
        g_bowlIndexBits[i].store(bits[i], std::memory_order_release);
    }
}

bool index_is_replaced_bowl(std::int32_t index) {
    if (index < 0 || index >= kMaxSourceRecords) return false;
    return (g_bowlIndexBits[static_cast<std::size_t>(index) / 64].load(
                std::memory_order_acquire) &
            (1ULL << (static_cast<std::size_t>(index) % 64))) != 0;
}

// Native geometry-generator ABI: four arguments. Do not reuse this
// function type for other native entry points.
using GeneratorFn = unsigned char(__fastcall*)(std::uintptr_t mapWidget,
                                                std::uintptr_t sourceContext,
                                                unsigned char rebuildMode,
                                                std::uintptr_t output);

GeneratorFn g_original = nullptr;
void* g_hookAddress = nullptr;
std::mutex g_logMutex;

struct ArrayHeader {
    std::uintptr_t data;
    std::int32_t num;
    std::int32_t max;
};
static_assert(sizeof(ArrayHeader) == 16);

struct Vector3D {
    double x, y, z;
};
static_assert(sizeof(Vector3D) == 24);

// Use a separate SEH-only helper to avoid unwinding C++ objects through __try.
bool safe_copy(void* dst, const void* src, std::size_t byteCount) noexcept {
    if (!dst || !src) return false;
    __try {
        std::memcpy(dst, src, byteCount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<class T>
bool read_at(std::uintptr_t address, T& out) noexcept {
    if (address == 0) return false;
    return safe_copy(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

void dll_location_anchor() { volatile int n = 1; (void)n; }
std::filesystem::path resolve_mod_folder() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&dll_location_anchor), &mod)) return {};
    wchar_t filename[32768]{};
    const DWORD n = GetModuleFileNameW(mod, filename, 32768);
    if (n == 0 || n >= 32768) return {};
    // mods/ResortMapBowlTweaks/dlls/main.dll -> mods/ResortMapBowlTweaks/
    return std::filesystem::path(filename).parent_path().parent_path();
}

std::filesystem::path output_path(const wchar_t* filename) {
    if (g_modDirectory.empty()) g_modDirectory = resolve_mod_folder();
    if (g_modDirectory.empty()) return {};
    const auto folder = g_modDirectory / L"logs";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return {};
    return folder / filename;
}

void write_status(const std::string& message) {
    if (!g_debugEnabled.load(std::memory_order_relaxed)) return;
    const auto path = output_path(L"ResortMapBowlTweaks.log");
    if (path.empty()) return;
    std::lock_guard<std::mutex> guard(g_logMutex);
    std::ofstream out(path, std::ios::app);
    if (out) out << message << '\n';
}

bool is_executable_address(const void* ptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    const DWORD basicProtection = mbi.Protect & 0xff;
    return basicProtection == PAGE_EXECUTE || basicProtection == PAGE_EXECUTE_READ ||
           basicProtection == PAGE_EXECUTE_READWRITE || basicProtection == PAGE_EXECUTE_WRITECOPY;
}

// Find functions in executable PE code sections only. RIP-relative addresses
// and branch destinations are wildcarded. Data layouts are checked separately.
struct SignatureByte { std::uint8_t value; bool wildcard; };
struct CodeSpan { const std::uint8_t* start; std::size_t length; };

bool parse_code_signature(const char* pattern, std::vector<SignatureByte>& result) {
    if (!pattern) return false;
    std::istringstream in(pattern);
    std::string token;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    while (in >> token) {
        if (token == "??") {
            result.push_back({0, true});
        } else if (token.size() == 2) {
            const int hi = nibble(token[0]);
            const int lo = nibble(token[1]);
            if (hi < 0 || lo < 0) return false;
            result.push_back({static_cast<std::uint8_t>((hi << 4) | lo), false});
        } else {
            return false;
        }
    }
    return result.size() >= 16 && !result.front().wildcard;
}

// Require one executable-code match across all sections. Use memchr to
// locate the signature's first byte, then verify the full byte pattern.
bool resolve_unique_signature(const char* name, const char* pattern,
                              const std::vector<CodeSpan>& spans,
                              std::uintptr_t moduleBase, std::uintptr_t& address) {
    std::vector<SignatureByte> signature;
    if (!parse_code_signature(pattern, signature)) {
        write_status(std::string("ERROR: Invalid signature: ") + name);
        return false;
    }
    unsigned matches = 0;
    const std::uint8_t* located = nullptr;
    for (const auto& span : spans) {
        if (span.length < signature.size()) continue;
        const auto* cursor = span.start;
        const auto* last = span.start + (span.length - signature.size());
        while (cursor <= last) {
            const auto bytesRemaining = static_cast<std::size_t>(last - cursor) + 1;
            const auto* at = static_cast<const std::uint8_t*>(
                std::memchr(cursor, signature.front().value, bytesRemaining));
            if (!at) break;
            bool matched = true;
            for (std::size_t i = 1; i < signature.size(); ++i) {
                if (!signature[i].wildcard && at[i] != signature[i].value) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                ++matches;
                located = at;
                if (matches > 1) break;
            }
            cursor = at + 1;
        }
        if (matches > 1) break;
    }
    if (matches != 1 || !is_executable_address(located)) {
        write_status(std::string("ERROR: Signature ") + name +
                     " did not resolve uniquely/executably; matches=" +
                     std::to_string(matches) + ". No hooks installed.");
        return false;
    }
    address = reinterpret_cast<std::uintptr_t>(located);
    write_status(std::string("Resolved ") + name + " at RVA=0x" + [&]() {
        std::ostringstream ss;
        ss << std::hex << (address - moduleBase);
        return ss.str();
    }());
    return true;
}

// Check instruction evidence for required offsets and record strides before
// installing hooks. A passing result is not a complete ABI/layout guarantee;
// compiler changes can reject a compatible build or miss other layout changes.
bool validate_native_layout_evidence(const ResolvedNativeFunctions& f) {
    int passed = 0;
    const auto require = [&](const char* name, std::uintptr_t address,
                             std::size_t window,
                             std::initializer_list<std::uint8_t> bytes) -> bool {
        if (!address || bytes.size() < 3 || window > 0x2000) return false;
        std::vector<std::uint8_t> code(window);
        if (!safe_copy(code.data(), reinterpret_cast<const void*>(address), code.size()) ||
            std::search(code.begin(), code.end(), bytes.begin(), bytes.end()) == code.end()) {
            write_status(std::string("ERROR: Native layout evidence missing: ") + name +
                         ". No hooks installed; please recheck this game update.");
            return false;
        }
        ++passed;
        return true;
    };
    // Geometry source records: table offset, stride, points, and closed flag.
    if (!require("source_context+0x70", f.generator, 0x580, {0x4C,0x8D,0x6B,0x70}) ||
        !require("geometry_stride_120", f.generator, 0x580, {0x48,0x6B,0xC8,0x78}) ||
        !require("geometry_points+0x18", f.generator, 0x580, {0x49,0x8B,0x7D,0x18}) ||
        !require("geometry_closed+0x28", f.generator, 0x580, {0x41,0x80,0x7D,0x28,0x00}))
        return false;
    // Map rebuild: label canvas, placement records, and geometry stride.
    if (!require("LabelCanvas+0x518", f.fullMapRebuild, 0x380, {0x49,0x8B,0x8D,0x18,0x05,0x00,0x00}) ||
        !require("placement_array+0x530", f.fullMapRebuild, 0x380, {0x4D,0x8D,0x8D,0x30,0x05,0x00,0x00}) ||
        !require("placement_stride_104", f.fullMapRebuild, 0x380, {0xB9,0x68,0x00,0x00,0x00}) ||
        !require("rebuild_geometry_stride_120", f.fullMapRebuild, 0x380, {0x48,0x6B,0xC8,0x78}))
        return false;
    // Map stroke installer and painter: stroke layer and record layout.
    if (!require("StrokeLayer+0x520", f.installMapStrokes, 0x240, {0x49,0x8B,0x8F,0x20,0x05,0x00,0x00}) ||
        !require("installed_stroke_stride_72", f.installMapStrokes, 0x240, {0x48,0x83,0xC7,0x48}) ||
        !require("stroke_array+0x328", f.paintLine, 0x900, {0x48,0x8D,0x99,0x28,0x03,0x00,0x00}))
        return false;
    write_status("LAYOUT native instruction evidence passed " +
                 std::to_string(passed) + "/11 checks (not a full ABI guarantee).");
    return true;
}

bool resolve_native_functions(ResolvedNativeFunctions& functions) {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        write_status("ERROR: Unable to determine executable name.");
        return false;
    }
    if (_wcsicmp(std::filesystem::path(exePath).filename().c_str(),
                 L"skieogame.exe") != 0) {
        write_status("ERROR: This DLL targets skieogame.exe only.");
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 4096) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96) return false;
    const auto imageSize = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    const auto* firstSection = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<const std::uint8_t*>(firstSection + nt->FileHeader.NumberOfSections) - base >
        nt->OptionalHeader.SizeOfHeaders) return false;
    std::vector<CodeSpan> spans;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = firstSection[i];
        if (!(s.Characteristics & IMAGE_SCN_CNT_CODE) ||
            !(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            s.VirtualAddress >= imageSize) continue;
        const std::size_t length = std::min<std::size_t>(
            s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData,
            imageSize - s.VirtualAddress);
        if (length >= 64) spans.push_back({base + s.VirtualAddress, length});
    }
    if (spans.empty()) {
        write_status("ERROR: Executable code sections not found.");
        return false;
    }
    // Patterns for supported native entry points. All must resolve uniquely,
    // including functions unused by the selected display settings.
    struct Target { const char* name; const char* signature; std::uintptr_t* output; };
    const Target targets[] = {
        {"bowl_geometry_generator",
         "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 70 FD FF FF "
         "48 81 EC 90 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 "
         "E0 01 00 00 48 8B 01 4D 8B E1 4C 89 4D 10 45 0F B6 F8 44 88 44 24 40",
         &functions.generator},
        {"map_stroke_painter",
         "4C 8B DC 55 53 56 57 41 56 49 8D 6B C1 48 81 EC C0 00 00 00 "
         "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 EF 41 83 79 08 02 49 8B F9 "
         "49 8B D8 44 8B F2 48 8B F1",
         &functions.paintLine},
        {"map_stroke_installer",
         "48 89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 "
         "41 57 48 83 EC 58 48 8D 99 40 05 00 00 4C 8B F2 83 7B 0C 00 "
         "4C 8D 63 0C 8B 53 08 4C 8B F9 48 8B 0B",
         &functions.installMapStrokes},
        {"full_map_rebuild",
         "4C 8B DC 55 49 8D AB 18 FB FF FF 48 81 EC E0 05 00 00 "
         "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 00 04 00 00 49 89 7B E8 "
         "48 8B FA 4D 89 6B D8 4C 8B E9 4D 89 73 D0 45 33 F6",
         &functions.fullMapRebuild},
        {"label_canvas_attachment",
         "45 33 C0 E9 08 00 00 00 CC CC CC CC CC CC CC CC "
         "40 55 56 57 48 83 EC 60 49 8B E8 48 8B F2 48 8B F9 48 85 D2 "
         "0F 84 ?? ?? ?? ?? 80 B9 88 01 00 00 00",
         &functions.addMapLabel},
    };
    for (const auto& t : targets) {
        if (!resolve_unique_signature(t.name, t.signature, spans,
                                      reinterpret_cast<std::uintptr_t>(base), *t.output))
            return false;
    }
    if (!validate_native_layout_evidence(functions)) return false;
    write_status("All native signatures and layout checks passed. PE timestamp=" +
                 std::to_string(nt->FileHeader.TimeDateStamp) +
                 ", image size=" + std::to_string(imageSize));
    if (nt->FileHeader.TimeDateStamp != 2061847679U || imageSize != 190664704U) {
        write_status("WARNING: Updated/unverified game build: instruction evidence passed, "
                     "but native layouts, UMG fields and hook ABI are not fully proven.");
    }
    return true;
}

const char* line_style_name(StrokeStyle style) {
    switch (style) {
    case StrokeStyle::Solid: return "solid";
    case StrokeStyle::Dashed: return "dashed";
    case StrokeStyle::Hidden: return "hidden";
    }
    return "unknown";
}

std::string lower_trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The colour values in the INI are sRGB hex (#RRGGBB). UMG's FLinearColor
// components are linear, so convert through the standard sRGB transfer curve.
// Hex values must NOT be truncated at '#' during INI comment parsing.
bool parse_hex_rgb(std::string input, std::array<float, 4>& output) {
    input = lower_trim(input);
    if (!input.empty() && input[0] == '#') input.erase(0, 1);
    if (input.size() != 6) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::array<float, 4> linear{0.f, 0.f, 0.f, 1.f};
    for (int i = 0; i < 3; ++i) {
        const int a = digit(input[2 * i]);
        const int b = digit(input[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        const float srgb = static_cast<float>(16 * a + b) / 255.f;
        linear[static_cast<std::size_t>(i)] = srgb <= 0.04045f
            ? srgb / 12.92f
            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }
    output = linear;
    return true;
}

// One simple INI file next to the DLL's dlls/ directory; read once per launch.
void load_mod_settings() {
    g_strokeStyle = StrokeStyle::Dashed;
    g_dashLength = 7.0;
    g_gapLength = 15.0;
    g_textStyle = TextStyle::Default;
    g_lightColor = {0.863157f, 0.799103f, 0.658375f, 1.f};
    g_darkColor = {0.019382f, 0.019382f, 0.024158f, 1.f};
    g_lightHex = "#EFE7D4";
    g_darkHex = "#26262B";
    g_debugEnabled.store(false, std::memory_order_relaxed);
    g_idLabelStyleEnabled = true;
    g_modDirectory = resolve_mod_folder();
    if (g_modDirectory.empty()) {
        write_status("WARNING: Cannot locate mod folder; defaults used.");
        return;
    }
    const auto path = g_modDirectory / L"ResortMapBowlTweaks.ini";
    std::ifstream in(path);
    if (!in) {
        write_status("WARNING: Missing " + path.string() + "; defaults used.");
        return;
    }
    // Read debug first, regardless of its position in the INI. Do not create
    // the logs folder, open a log file, or write anything unless enabled.
    std::vector<std::string> iniLines;
    std::string line;
    while (std::getline(in, line)) iniLines.push_back(line);
    for (auto entry : iniLines) {
        const auto comment = entry.find(';');
        if (comment != std::string::npos) entry.resize(comment);
        entry = lower_trim(entry);
        if (entry.empty() || entry.front() == '#') continue;
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        if (lower_trim(entry.substr(0, eq)) != "debug") continue;
        const auto value = lower_trim(entry.substr(eq + 1));
        g_debugEnabled.store(value == "true" || value == "1" || value == "yes",
                             std::memory_order_relaxed);
    }
    if (g_debugEnabled.load(std::memory_order_relaxed)) {
        const auto logPath = output_path(L"ResortMapBowlTweaks.log");
        if (!logPath.empty()) {
            std::lock_guard<std::mutex> guard(g_logMutex);
            std::ofstream reset(logPath, std::ios::trunc);
            if (reset) reset << "ResortMapBowlTweaks 1.0.0\n";
        }
    }
    for (auto line : iniLines) {
        // ';' starts an inline comment. '#' starts a full-line comment,
        // but is otherwise part of a colour value such as #EFE7D4.
        const auto comment = line.find(';');
        if (comment != std::string::npos) line.resize(comment);
        line = lower_trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line.front() == '[') continue;
        const std::string key = lower_trim(line.substr(0, eq));
        const std::string val = lower_trim(line.substr(eq + 1));
        if (key == "debug") {
            continue; // Already read on the first pass.
        } else if (key == "id_label_style") {
            if (val == "true" || val == "1" || val == "yes") g_idLabelStyleEnabled = true;
            else if (val == "false" || val == "0" || val == "no") g_idLabelStyleEnabled = false;
            else write_status("WARNING: Invalid id_label_style value: " + val);
        } else if (key == "lines") {
            if (val == "dashed") g_strokeStyle = StrokeStyle::Dashed;
            else if (val == "solid") g_strokeStyle = StrokeStyle::Solid;
            else if (val == "hidden") g_strokeStyle = StrokeStyle::Hidden;
            else write_status("WARNING: Invalid lines setting: " + val);
        } else if (key == "trail_name_text") {
            if (val == "default" || val == "yellow") g_textStyle = TextStyle::Default;
            else if (val == "inverted") g_textStyle = TextStyle::Inverted;
            else if (val == "box") g_textStyle = TextStyle::WhitePlate;
            else write_status("WARNING: Invalid trail_name_text setting: " + val);
        } else if (key == "light_color" || key == "dark_color" ||
                   key == "light_colour" || key == "dark_colour") {
            std::array<float, 4> parsed{};
            if (!parse_hex_rgb(val, parsed)) {
                write_status("WARNING: Invalid " + key + " value: " + val +
                             " (expected #RRGGBB)");
            } else if (key == "light_color" || key == "light_colour") {
                g_lightColor = parsed;
                g_lightHex = val;
            } else {
                g_darkColor = parsed;
                g_darkHex = val;
            }
        } else if (key == "dash_length" || key == "gap_length") {
            try {
                const double n = std::stod(val);
                if (!std::isfinite(n)) continue;
                if (key == "dash_length" && n >= 3 && n <= 30) g_dashLength = n;
                else if (key == "gap_length" && n >= 4 && n <= 60) g_gapLength = n;
                else write_status("WARNING: Out-of-range " + key + "=" + val);
            } catch (...) { write_status("WARNING: Invalid number for " + key); }
        }
    }
    write_status("INI file: " + path.string());
    write_status(std::string("Settings: lines=") + line_style_name(g_strokeStyle) +
                 ", dash=" + std::to_string(g_dashLength) +
                 ", gap=" + std::to_string(g_gapLength) +
                 ", trail_name_text=" +
                 (g_textStyle == TextStyle::Inverted ? "inverted" :
                  g_textStyle == TextStyle::WhitePlate ? "box" : "default") +
                 ", light_color=" + g_lightHex + ", dark_color=" + g_darkHex +
                 ", id_label_style=" + (g_idLabelStyleEnabled ? "true" : "false") +
                 ", debug=" + (g_debugEnabled.load() ? "true" : "false"));
}

// The paint helper receives a pointer into the live UResortMapViewStrokeLayer's
// 72-byte records. Verify the entire relationship instead of treating an
// arbitrary pointer as a stroke record. It also prevents affecting other UI.
bool try_painted_bowl_record(std::uintptr_t colorPtr, std::int32_t& index) {
    const auto layer = g_mapStrokeLayer.load(std::memory_order_acquire);
    if (!layer || !colorPtr) return false;
    ArrayHeader strokes{};
    if (!read_at(layer + 0x328, strokes) || !strokes.data || strokes.num < 1 ||
        strokes.num > 16384 || strokes.max < strokes.num) return false;
    if (colorPtr < strokes.data + 32) return false;
    const std::uintptr_t offset = colorPtr - strokes.data - 32;
    if (offset % 72 != 0 || offset / 72 >= static_cast<std::size_t>(strokes.num))
        return false;
    if (!read_at(colorPtr + 24, index)) return false; // record +0x38
    return index_is_replaced_bowl(index);
}

// Render each dash through the *original* game painter. The points here are
// 2-D map-local coordinates after native projection, so dash/gap lengths do
// not depend on the stored 3-D path or which camera direction was selected.
// Each original call owns only its stack TArray view, as in the game painter.
void __fastcall hooked_paint_line(std::uintptr_t elementList,
                                  std::uintptr_t layerId,
                                  std::uintptr_t geometry,
                                  std::uintptr_t pointArray,
                                  std::uintptr_t colorPtr,
                                  float thickness, float opacity,
                                  unsigned char highlight) {
    if (!g_originalPaintLine) return;
    const auto original = [&]() {
        g_originalPaintLine(elementList, layerId, geometry, pointArray,
                            colorPtr, thickness, opacity, highlight);
    };
    if (g_strokeStyle == StrokeStyle::Solid) {
        original();
        return;
    }
    std::int32_t sourceIndex = -1;
    if (!try_painted_bowl_record(colorPtr, sourceIndex)) {
        original();
        return;
    }
    if (g_strokeStyle == StrokeStyle::Hidden) {
        // Secondary safety net; the generated bowl stroke point arrays are
        // also blanked before they reach the map painter.
        return;
    }
    if (thickness >= 12.0f) { // Leave special end-cap elements alone in dashed mode.
        original();
        return;
    }

    ArrayHeader points{};
    if (!read_at(pointArray, points) || points.num < 2 || points.num > 4096 ||
        points.max < points.num || !points.data) {
        original();
        return;
    }
    std::vector<Vector2D> path(static_cast<std::size_t>(points.num));
    if (!safe_copy(path.data(), reinterpret_cast<const void*>(points.data),
                   path.size() * sizeof(Vector2D))) {
        original();
        return;
    }
    double length = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        for (const auto& p : {path[i - 1], path[i]}) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                std::abs(p.x) > 1e9 || std::abs(p.y) > 1e9) {
                original();
                return;
            }
        }
        length += std::hypot(path[i].x - path[i - 1].x,
                             path[i].y - path[i - 1].y);
    }
    if (!std::isfinite(length) || length < 1.0) {
        original();
        return;
    }

    // Bound drawing cost when viewing a very large map at extreme zoom.
    const double scale = std::max(1.0, length /
        (kMaximumDashesPerStroke * (g_dashLength + g_gapLength)));
    const double dash = g_dashLength * scale;
    const double period = (g_dashLength + g_gapLength) * scale;
    std::vector<Vector2D> segment;
    segment.reserve(path.size() + 2);
    int emitted = 0;
    const auto flush = [&]() {
        if (segment.size() < 2) { segment.clear(); return; }
        ArrayHeader dashPoints{reinterpret_cast<std::uintptr_t>(segment.data()),
                               static_cast<std::int32_t>(segment.size()),
                               static_cast<std::int32_t>(segment.size())};
        g_originalPaintLine(elementList, layerId, geometry,
            reinterpret_cast<std::uintptr_t>(&dashPoints), colorPtr,
            thickness, opacity, highlight);
        ++emitted;
        segment.clear();
    };
    double phase = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Vector2D a = path[i - 1], b = path[i];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-8) continue;
        double pos = 0.0;
        while (pos < len - 1e-8) {
            const bool ink = phase < dash - 1e-8;
            const double boundary = ink ? dash : period;
            const double step = std::min(len - pos, boundary - phase);
            if (step <= 1e-8) {
                phase = ink ? dash : 0.0;
                if (ink) flush();
                continue;
            }
            if (ink) {
                const auto lerp = [&](double along) -> Vector2D {
                    const double t = along / len;
                    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                };
                if (segment.empty()) segment.push_back(lerp(pos));
                segment.push_back(lerp(pos + step));
            }
            pos += step;
            phase += step;
            if (phase >= dash - 1e-8 && phase < period - 1e-8) {
                if (ink) flush();
            }
            if (phase >= period - 1e-8) phase = 0.0;
        }
    }
    flush();
    if (emitted == 0) { original(); return; }
    const auto count = g_dashedStrokes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8) {
        write_status("Dashed map stroke " + std::to_string(count) +
                     ": source_index=" + std::to_string(sourceIndex) +
                     ", dash_elements=" + std::to_string(emitted) +
                     ", path_points=" + std::to_string(points.num) +
                     ", length=" + std::to_string(static_cast<int>(length)));
    }
}

// Expected UTextBlock and FSlateFontInfo layout for the supported build:
// TextBlock.Font = +0x1E8; Font.OutlineSettings = +0x10;
// OutlineSettings.OutlineColor = +0x10; OutlineSize = +0x00.
// Prefer invoking reflected UMG setters over modifying the live widget.
struct SlateColorParam {
    float r, g, b, a;
    std::uint8_t colorUseRule = 0; // UseColor_Specified
    std::uint8_t pad[3]{};
};
static_assert(sizeof(SlateColorParam) == 20);

// Get the displayed string, without copying FText's opaque engine-owned
// reference-counted representation. FText::ToFString is supplied by UE4SS.
// A bounded copy protects against unexpectedly long/malformed name fields.
std::wstring ftext_to_label_name(const RC::Unreal::FText* text) {
    if (!text) return {};
    const auto& value = text->ToFString();
    const auto* chars = *value;
    if (!chars) return {};
    std::size_t len = 0;
    while (len < 256 && chars[len] != L'\0') ++len;
    if (len == 0 || len == 256) return {};
    std::wstring name(chars, len);
    // Normalize case and surrounding whitespace, not punctuation: avoid
    // accidentally matching two different trail names.
    const auto ws = [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; };
    while (!name.empty() && ws(name.front())) name.erase(name.begin());
    while (!name.empty() && ws(name.back())) name.pop_back();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return name;
}

// The supported geometry record stores an FText field at offset +0x30.
// Collect ALL bowls, independently of the line-visibility configuration.
// We only attempt FText conversion after verifying shape, closed flag, and
// a sane polygon point count. If no names can be read, custom styling is skipped.
void refresh_bowl_names(std::uintptr_t sourceContext) {
    if (!sourceContext) return;
    ArrayHeader source{};
    if (!read_at(sourceContext + 112, source) || !source.data ||
        source.num <= 0 || source.num > kMaxSourceRecords || source.max < source.num)
        return;
    std::unordered_set<std::wstring> collected;
    int bowlCount = 0, unreadable = 0;
    for (int i = 0; i < source.num; ++i) {
        const auto record = source.data + static_cast<std::size_t>(i) * kSourceRecordBytes;
        std::uint8_t shape = 0, closed = 0;
        ArrayHeader points{};
        if (!read_at(record, shape) || !read_at(record + 40, closed) ||
            !read_at(record + 24, points)) continue;
        if (shape != 0 || closed != 1 || points.num < 8 ||
            points.num > kMaxCenterlineBowlPoints || points.max < points.num || !points.data)
            continue;
        ++bowlCount;
        // The game owns this FText for the duration of the synchronous call.
        const auto* nameText = reinterpret_cast<const RC::Unreal::FText*>(record + 48);
        const auto name = ftext_to_label_name(nameText);
        if (name.empty()) ++unreadable;
        else collected.emplace(name);
    }
    const auto uniqueNameCount = collected.size();
    {
        std::lock_guard<std::mutex> lock(g_bowlNamesMutex);
        g_bowlNames.swap(collected);
    }
    const auto update = g_bowlNamesUpdates.fetch_add(1, std::memory_order_relaxed) + 1;
    if (update <= 4) {
        write_status("Bowl name scan " + std::to_string(update) +
                     ": closed_bowls=" + std::to_string(bowlCount) +
                     ", unique_names=" + std::to_string(uniqueNameCount) +
                     ", unreadable=" + std::to_string(unreadable));
    }
}

// 'default' uses dark text with a light outline; 'inverted' uses light
// text with a dark outline. Only identified bowl TextBlocks are modified.
bool apply_outlined_map_text(std::uintptr_t ptr, bool inverted) {
    if (!ptr || !g_textBlockClass || !g_setFontFn || !g_setColorFn) return false;
    auto* widget = reinterpret_cast<RC::Unreal::UObject*>(ptr);
    if (!widget->IsA(g_textBlockClass)) return false;

    auto* originalFont = widget->GetValuePtrByPropertyNameInChain<void>(STR("Font"));
    if (!originalFont) return false;
    alignas(16) std::array<std::byte, 0x60> font{};
    if (!safe_copy(font.data(), originalFont, font.size())) return false;
    std::int32_t outlineSize = 0;
    std::memcpy(&outlineSize, font.data() + 0x10, sizeof(outlineSize));
    if (outlineSize < 0 || outlineSize > 16) return false;
    outlineSize = std::max(outlineSize, 2);
    std::memcpy(font.data() + 0x10, &outlineSize, sizeof(outlineSize));
    const auto& outline = inverted ? g_darkColor : g_lightColor;
    std::memcpy(font.data() + 0x20, outline.data(), sizeof(float) * 4);
    const auto& fill = inverted ? g_lightColor : g_darkColor;
    SlateColorParam textColour{fill[0], fill[1], fill[2], fill[3], 0, {0,0,0}};
    widget->ProcessEvent(g_setFontFn, font.data());
    widget->ProcessEvent(g_setColorFn, &textColour);
    return true;
}

// Use the game's own UMG Border layout primitive, as with the resort's
// rectangular location-name plates. The bowl TextBlock becomes a descendant
// of the plate, not a detached sibling. The external CanvasPanelSlot remains
// associated with the whole plate, so map transforms affect them together.
// Dark outer border (1px), light inner plate and dark glyphs.
struct LinearColorParam { float r, g, b, a; };
struct MarginParam { float left, top, right, bottom; };
static_assert(sizeof(MarginParam) == 16);
struct CanvasAddChildParams {
    RC::Unreal::UObject* content;
    RC::Unreal::UObject* result;
};
static_assert(sizeof(CanvasAddChildParams) == 16);
struct RemoveChildParams {
    RC::Unreal::UObject* content;
    std::uint8_t result;
    std::uint8_t padding[7]{};
};
static_assert(sizeof(RemoveChildParams) == 16);

RC::Unreal::UObject* create_plate_widget(RC::Unreal::UClass* klass,
                                          RC::Unreal::UObject* outer) {
    if (!klass || !outer) return nullptr;
    RC::Unreal::FStaticConstructObjectParameters params{klass, outer};
    return RC::Unreal::UObjectGlobals::StaticConstructObject(params);
}

// FSlateBrush offsets confirmed by the game object's reflected layout.
// RoundedBox with zero corner radii renders a filled square rectangle;
// two concentric borders implement a thin dark edge without relying on
// potentially platform-specific SlateBrushOutlineSettings behaviour.
bool set_plate_brush(RC::Unreal::UObject* border, bool black) {
    if (!border || !g_borderSetBrushFn) return false;
    const auto* original = border->GetValuePtrByPropertyNameInChain<void>(STR("Background"));
    if (!original) return false;
    alignas(16) std::array<std::byte, 0xB0> brush{};
    if (!safe_copy(brush.data(), original, brush.size())) return false;
    brush[0x1C] = std::byte{4}; // ESlateBrushDrawType::RoundedBox
    const auto& rgb = black ? g_darkColor : g_lightColor;
    const LinearColorParam tint{rgb[0], rgb[1], rgb[2], rgb[3]};
    std::memcpy(brush.data() + 0x08, &tint, sizeof(tint));
    brush[0x18] = std::byte{0}; // ESlateColorStylingMode::UseColor_Specified
    std::memset(brush.data() + 0x40, 0, 0x20); // square corners
    const float noExtraOutline = 0.f;
    std::memcpy(brush.data() + 0x74, &noExtraOutline, sizeof(noExtraOutline));
    border->ProcessEvent(g_borderSetBrushFn, brush.data());
    return true;
}

struct BoxResult {
    bool success = false;
    int canvasDepth = 0;
    std::string method;
    std::string failure;
};

// Used for both UBorder::SetContent and UContentWidget::SetContent.
bool set_content_checked(RC::Unreal::UObject* host, RC::Unreal::UFunction* fn,
                         RC::Unreal::UObject* content) {
    if (!host || !fn) return false;
    CanvasAddChildParams args{content, nullptr};
    host->ProcessEvent(fn, &args);
    return args.result != nullptr;
}

BoxResult apply_boxed_map_text(RC::Unreal::UObject* textWidget) {
    BoxResult result{};
    if (!textWidget || !g_borderClass || !g_getParentFn ||
        !g_borderSetContentFn || !g_borderSetBrushFn ||
        !g_borderSetPaddingFn || !g_setFontFn || !g_setColorFn ||
        !g_contentWidgetClass || !g_contentSetContentFn) {
        result.failure = "reflection_missing";
        return result;
    }
    const auto labelAddress = reinterpret_cast<std::uintptr_t>(textWidget);
    std::uintptr_t cachedInnerBorder = 0;
    {
        std::lock_guard<std::mutex> lock(g_plateMutex);
        const auto found = g_platedTextBlocks.find(labelAddress);
        if (found != g_platedTextBlocks.end()) cachedInnerBorder = found->second;
    }
    // An existing bowl widget can have its font reset when the map refreshes.
    // Only trust a cached box if its actual parent still matches the exact
    // UBorder we created. This also guards against recycled UObject addresses.
    if (cachedInnerBorder) {
        RC::Unreal::UObject* currentParent = nullptr;
        textWidget->ProcessEvent(g_getParentFn, &currentParent);
        if (reinterpret_cast<std::uintptr_t>(currentParent) == cachedInnerBorder &&
            currentParent->IsA(g_borderClass)) {
            auto* oldFont = textWidget->GetValuePtrByPropertyNameInChain<void>(STR("Font"));
            if (!oldFont) { result.failure = "cached_font_missing"; return result; }
            alignas(16) std::array<std::byte, 0x60> cachedFont{};
            if (!safe_copy(cachedFont.data(), oldFont, cachedFont.size())) {
                result.failure = "cached_font_copy_failed";
                return result;
            }
            const std::int32_t zero = 0;
            std::memcpy(cachedFont.data() + 0x10, &zero, sizeof(zero));
            SlateColorParam darkText{g_darkColor[0], g_darkColor[1],
                                     g_darkColor[2], g_darkColor[3], 0, {0,0,0}};
            textWidget->ProcessEvent(g_setFontFn, cachedFont.data());
            textWidget->ProcessEvent(g_setColorFn, &darkText);
            result.success = true;
            result.method = "already_wrapped";
            return result;
        }
        std::lock_guard<std::mutex> lock(g_plateMutex);
        g_platedTextBlocks.erase(labelAddress);
    }

    auto* originalFont = textWidget->GetValuePtrByPropertyNameInChain<void>(STR("Font"));
    if (!originalFont) {
        result.failure = "text_font_missing";
        return result;
    }
    alignas(16) std::array<std::byte, 0x60> font{};
    if (!safe_copy(font.data(), originalFont, font.size())) {
        result.failure = "font_copy_failed";
        return result;
    }
    const std::int32_t noGlyphOutline = 0;
    std::memcpy(font.data() + 0x10, &noGlyphOutline, sizeof(noGlyphOutline));

    RC::Unreal::UObject* parent = nullptr;
    textWidget->ProcessEvent(g_getParentFn, &parent);
    if (!parent || parent == textWidget) {
        result.failure = "no_parent";
        return result;
    }

    // Make the inner light-coloured plate first. The
    // label itself will be inserted INSIDE this container.
    auto* inner = create_plate_widget(g_borderClass, parent);
    if (!inner || !set_plate_brush(inner, false)) {
        result.failure = "inner_creation_failed";
        return result;
    }
    MarginParam platePadding{3.f, 1.5f, 3.f, 1.5f};
    inner->ProcessEvent(g_borderSetPaddingFn, &platePadding);

    const bool existingOuter = parent->IsA(g_borderClass);
    RC::Unreal::UObject* outer = existingOuter ? parent :
        create_plate_widget(g_borderClass, parent);
    if (!outer || !set_plate_brush(outer, true)) {
        result.failure = "outer_creation_failed";
        return result;
    }
    MarginParam darkEdge{1.f, 1.f, 1.f, 1.f};
    outer->ProcessEvent(g_borderSetPaddingFn, &darkEdge);

    // The original widget is still parented. Do not reparent it until the
    // new outer border has been placed in its former parent slot.
    if (existingOuter) {
        if (!set_content_checked(outer, g_borderSetContentFn, inner)) {
            result.failure = "existing_border_replace_failed";
            return result;
        }
        if (!set_content_checked(inner, g_borderSetContentFn, textWidget)) {
            set_content_checked(outer, g_borderSetContentFn, textWidget);
            result.failure = "existing_border_text_attach_failed";
            return result;
        }
        result.method = "existing_border";
    } else if (g_canvasPanelClass && parent->IsA(g_canvasPanelClass)) {
        if (!g_canvasSlotClass || !g_canvasAddChildFn || !g_panelRemoveChildFn ||
            !g_canvasSlotSetLayoutFn || !g_canvasSlotSetAutoSizeFn ||
            !g_canvasSlotSetZOrderFn) {
            result.failure = "canvas_api_missing";
            return result;
        }
        std::uintptr_t oldSlot = 0;
        if (!read_at(labelAddress + 0x30, oldSlot) || !oldSlot ||
            !reinterpret_cast<RC::Unreal::UObject*>(oldSlot)->IsA(g_canvasSlotClass)) {
            result.failure = "label_canvas_slot_missing";
            return result;
        }
        std::array<std::byte, 0x40> layout{};
        std::uint8_t autoSize = 0;
        std::int32_t zOrder = 0;
        if (!safe_copy(layout.data(), reinterpret_cast<void*>(oldSlot + 0x38), layout.size()) ||
            !read_at(oldSlot + 0x78, autoSize) || !read_at(oldSlot + 0x7C, zOrder)) {
            result.failure = "canvas_slot_read_failed";
            return result;
        }

        RemoveChildParams remove{textWidget, 0, {}};
        parent->ProcessEvent(g_panelRemoveChildFn, &remove);
        if (!remove.result) {
            result.failure = "canvas_remove_failed";
            return result;
        }
        CanvasAddChildParams add{outer, nullptr};
        parent->ProcessEvent(g_canvasAddChildFn, &add);
        if (!add.result) {
            // Restore the game's original label rather than leave it missing.
            CanvasAddChildParams restore{textWidget, nullptr};
            parent->ProcessEvent(g_canvasAddChildFn, &restore);
            if (restore.result) {
                restore.result->ProcessEvent(g_canvasSlotSetLayoutFn, layout.data());
                restore.result->ProcessEvent(g_canvasSlotSetAutoSizeFn, &autoSize);
                restore.result->ProcessEvent(g_canvasSlotSetZOrderFn, &zOrder);
            }
            result.failure = "canvas_outer_attach_failed";
            return result;
        }
        add.result->ProcessEvent(g_canvasSlotSetLayoutFn, layout.data());
        add.result->ProcessEvent(g_canvasSlotSetAutoSizeFn, &autoSize);
        add.result->ProcessEvent(g_canvasSlotSetZOrderFn, &zOrder);
        if (!set_content_checked(outer, g_borderSetContentFn, inner) ||
            !set_content_checked(inner, g_borderSetContentFn, textWidget)) {
            RemoveChildParams removeOuter{outer, 0, {}};
            parent->ProcessEvent(g_panelRemoveChildFn, &removeOuter);
            CanvasAddChildParams restore{textWidget, nullptr};
            parent->ProcessEvent(g_canvasAddChildFn, &restore);
            if (restore.result) {
                restore.result->ProcessEvent(g_canvasSlotSetLayoutFn, layout.data());
                restore.result->ProcessEvent(g_canvasSlotSetAutoSizeFn, &autoSize);
                restore.result->ProcessEvent(g_canvasSlotSetZOrderFn, &zOrder);
            }
            result.failure = "canvas_contents_attach_failed";
            return result;
        }
        result.method = "canvas_wrapped";
    } else if (parent->IsA(g_contentWidgetClass)) {
        // Most nested labels are inside a SizeBox/Border-like single-child
        // container. Replacing its content preserves that parent's map slot.
        if (!set_content_checked(parent, g_contentSetContentFn, outer)) {
            result.failure = "content_outer_attach_failed";
            return result;
        }
        if (!set_content_checked(outer, g_borderSetContentFn, inner) ||
            !set_content_checked(inner, g_borderSetContentFn, textWidget)) {
            set_content_checked(parent, g_contentSetContentFn, textWidget);
            result.failure = "content_contents_attach_failed";
            return result;
        }
        result.method = "content_wrapped";
    } else {
        // Never wrap a generic multi-child panel (horizontal box, overlay,
        // etc.) unless its slot positioning can be preserved explicitly.
        // Failing closed keeps the original map labels readable.
        result.failure = "parent_not_safe_to_wrap";
        return result;
    }

    // Update the ORIGINAL UTextBlock. The game keeps its UObject pointer,
    // name, font metrics and text properties; only the parent is a UBorder.
    SlateColorParam darkText{g_darkColor[0], g_darkColor[1],
                             g_darkColor[2], g_darkColor[3], 0, {0,0,0}};
    textWidget->ProcessEvent(g_setFontFn, font.data());
    textWidget->ProcessEvent(g_setColorFn, &darkText);
    {
        std::lock_guard<std::mutex> lock(g_plateMutex);
        g_platedTextBlocks[labelAddress] = reinterpret_cast<std::uintptr_t>(inner);
    }
    result.success = true;
    return result;
}

// GetChildrenCount and GetChildAt are reflected UPanelWidget methods.
// Traverse ONLY children of the Resort Map's LabelCanvas. A label might be
// nested in a SizeBox/Overlay rather than itself being a UTextBlock.
struct GetPanelChildParams {
    std::int32_t index;
    std::int32_t pad;
    RC::Unreal::UObject* result;
};
static_assert(sizeof(GetPanelChildParams) == 16);

struct LabelPassStats {
    int visited = 0;
    int textBlocks = 0;
    int styled = 0;
    int bowlMatches = 0;
    int noName = 0;
    int panelWidgets = 0;
    int userWidgets = 0;
    int richTextBlocks = 0;
    int boxFailures = 0;
    int boxesNested = 0;
    int boxesDirect = 0;
    int boxesExisting = 0;
    std::vector<RC::Unreal::UObject*> pendingBoxLabels;
};

void restyle_label_widget(RC::Unreal::UObject* widget, int depth,
                          LabelPassStats& stats,
                          const std::unordered_set<std::wstring>& bowlNames,
                          bool nativeIdProven = false) {
    if (!widget || depth > 7 || stats.visited >= 700) return;
    ++stats.visited;
    if (widget->IsA(g_textBlockClass)) {
        ++stats.textBlocks;
        // Name-based mode only: match the displayed text against known bowls.
        // Unrelated label widgets must retain their native appearance.
        auto* text = widget->GetValuePtrByPropertyNameInChain<RC::Unreal::FText>(STR("Text"));
        const auto name = ftext_to_label_name(text);
        if (name.empty()) {
            ++stats.noName;
        } else if (nativeIdProven || bowlNames.find(name) != bowlNames.end()) {
            // nativeIdProven applies only to subtrees rooted at independently
            // verified game-created label widgets, never a whole map canvas.
            ++stats.bowlMatches;
            if (g_textStyle == TextStyle::Default ||
                g_textStyle == TextStyle::Inverted) {
                if (apply_outlined_map_text(reinterpret_cast<std::uintptr_t>(widget),
                                            g_textStyle == TextStyle::Inverted))
                    ++stats.styled;
            } else if (g_textStyle == TextStyle::WhitePlate) {
                // Defer canvas mutations until AFTER child enumeration.
                stats.pendingBoxLabels.push_back(widget);
            }
        }
        return;
    }
    if (g_richTextBlockClass && widget->IsA(g_richTextBlockClass))
        ++stats.richTextBlocks; // diagnostic; needs a separate style pathway

    if (g_panelWidgetClass && g_getChildrenCountFn && g_getChildAtFn &&
        widget->IsA(g_panelWidgetClass)) {
        ++stats.panelWidgets;
        std::int32_t numChildren = 0;
        widget->ProcessEvent(g_getChildrenCountFn, &numChildren);
        if (numChildren >= 0 && numChildren <= 600) {
            for (int i = 0; i < numChildren && stats.visited < 700; ++i) {
                GetPanelChildParams child{i, 0, nullptr};
                widget->ProcessEvent(g_getChildAtFn, &child);
                if (child.result && child.result != widget)
                    restyle_label_widget(child.result, depth + 1, stats, bowlNames,
                                         nativeIdProven);
            }
        }
    } else if (g_userWidgetClass && widget->IsA(g_userWidgetClass)) {
        ++stats.userWidgets;
        // UUserWidget.WidgetTree and UWidgetTree.RootWidget offsets are
        // 0x2C0 and 0x30 for the supported UE5.8 build.
        std::uintptr_t tree = 0;
        std::uintptr_t root = 0;
        if (read_at(reinterpret_cast<std::uintptr_t>(widget) + 0x2C0, tree) && tree &&
            read_at(tree + 0x30, root) && root &&
            root != reinterpret_cast<std::uintptr_t>(widget)) {
            restyle_label_widget(reinterpret_cast<RC::Unreal::UObject*>(root),
                                 depth + 1, stats, bowlNames, nativeIdProven);
        }
    }
}

// In name-matching mode, apply text colours before attachment. Box wrapping
// occurs after the game establishes the label's final canvas placement.
bool prime_box_text(RC::Unreal::UObject* textWidget) {
    if (!textWidget || !g_setFontFn || !g_setColorFn) return false;
    auto* originalFont = textWidget->GetValuePtrByPropertyNameInChain<void>(STR("Font"));
    if (!originalFont) return false;
    alignas(16) std::array<std::byte, 0x60> font{};
    if (!safe_copy(font.data(), originalFont, font.size())) return false;
    const std::int32_t noOutline = 0;
    std::memcpy(font.data() + 0x10, &noOutline, sizeof(noOutline));
    SlateColorParam darkText{g_darkColor[0], g_darkColor[1],
                             g_darkColor[2], g_darkColor[3], 0, {0,0,0}};
    textWidget->ProcessEvent(g_setFontFn, font.data());
    textWidget->ProcessEvent(g_setColorFn, &darkText);
    return true;
}

// Native map labels pass through this attachment helper. The callback
// records identity or primes text styling before the canvas receives each label.
// The callback never suppresses or replaces UWidgets: it leaves layout,
// ownership and source-index-associated native placement with the game.
std::uintptr_t __fastcall hooked_add_map_label(std::uintptr_t panel,
                                                std::uintptr_t child) {
    if (panel && child && g_captureMapAttachments.load(std::memory_order_relaxed) &&
        panel == g_activeLabelCanvas.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_mapAttachmentMutex);
        // Only read InternalIndex from the actual, live UWidget
        // supplied by the game. Cap capture in pathological rebuilds.
        std::int32_t internalIndex = -1;
        if (g_mapAttachmentByObjectIndex.size() < 2048 &&
            read_at(child + 0xC, internalIndex) &&
            internalIndex > 0 && internalIndex < 4000000) {
            auto [it, inserted] = g_mapAttachmentByObjectIndex.emplace(
                static_cast<std::uint32_t>(internalIndex), child);
            if (!inserted && it->second != child) it->second = 0; // ambiguous
        }
    }
    if (!g_idLabelStyleEnabled && panel && child && g_originalAddMapLabel &&
        panel == g_activeLabelCanvas.load(std::memory_order_relaxed) &&
        g_textBlockClass && g_setFontFn && g_setColorFn) {
        auto* widget = reinterpret_cast<RC::Unreal::UObject*>(child);
        if (widget->IsA(g_textBlockClass)) {
            auto* text = widget->GetValuePtrByPropertyNameInChain<RC::Unreal::FText>(STR("Text"));
            const auto name = ftext_to_label_name(text);
            bool isBowl = false;
            if (!name.empty()) {
                std::lock_guard<std::mutex> lock(g_bowlNamesMutex);
                isBowl = g_bowlNames.find(name) != g_bowlNames.end();
            }
            if (isBowl) {
                const bool prepared = g_textStyle == TextStyle::WhitePlate
                    ? prime_box_text(widget)
                    : apply_outlined_map_text(child, g_textStyle == TextStyle::Inverted);
                if (prepared) {
                    const auto count = g_preparedLabelCount.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    if (count <= 8)
                        write_status("Prepared bowl label before canvas attachment: " +
                                     std::to_string(count));
                }
            }
        }
    }
    // Do not reparent or replace the game-owned label widget here.
    return g_originalAddMapLabel(panel, child);
}

// Label placement depends on native stroke point/flag arrays. Hidden-line
// mode must preserve these arrays until after label positioning.
int suppress_hidden_bowl_strokes(
    std::uintptr_t installedStrokeArray,
    const std::array<std::uint64_t, 8>& replacedIndices);

void apply_map_label_style_pass(std::uintptr_t mapWidget, const char* stage);

// Called by the native map renderer after adding labels to LabelCanvas.
std::uintptr_t __fastcall hooked_install_map_strokes(std::uintptr_t mapWidget,
                                                      std::uintptr_t output) {
    const auto result = g_originalInstallMapStrokes(mapWidget, output);
    if (!mapWidget) return result;

    // The installer runs after label creation and transfers its native-owned
    // 72-byte stroke records to StrokeLayer (stroke array at +0x328).
    // Blanking only the installed layer's bowl stroke Num fields prevents
    // its line/circle painter from drawing bowls while preserving the
    // generator output that the label placement algorithm just consumed.
    if (g_strokeStyle == StrokeStyle::Hidden) {
        std::uintptr_t layer = 0;
        int suppressed = -1;
        if (read_at(mapWidget + 0x520, layer) && layer) {
            std::array<std::uint64_t, 8> indices{};
            for (std::size_t i = 0; i < indices.size(); ++i)
                indices[i] = g_bowlIndexBits[i].load(std::memory_order_acquire);
            suppressed = suppress_hidden_bowl_strokes(layer + 0x328, indices);
        }
        const auto call = g_hiddenStrokeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call <= 12) {
            write_status("Hidden installed stroke layer " + std::to_string(call) +
                         ": suppressed_bowl_strokes=" + std::to_string(suppressed) +
                         ", layer_found=" + std::to_string(layer != 0));
        }
    }

    apply_map_label_style_pass(mapWidget, "stroke-install");
    return result;
}

// Name-based identification mode. ID-based styling runs after full rebuild.
// Rechecking here catches overwritten text; the box cache avoids duplication.
void apply_map_label_style_pass(std::uintptr_t mapWidget, const char* stage) {
    // In ID mode, wait for the native rebuild to finish and produce its
    // geometry-index / UObject-handle associations. Do not guess by name.
    if (g_idLabelStyleEnabled) return;
    if (!mapWidget) return;
    std::uintptr_t canvasPtr = 0;
    if (!read_at(mapWidget + 0x518, canvasPtr) || !canvasPtr ||
        !g_panelWidgetClass || !g_getChildrenCountFn || !g_getChildAtFn ||
        !g_textBlockClass) return;

    std::unordered_set<std::wstring> bowlNames;
    {
        std::lock_guard<std::mutex> lock(g_bowlNamesMutex);
        bowlNames = g_bowlNames;
    }
    // Fail closed if no bowl names could be extracted from the current map.
    if (bowlNames.empty()) return;
    LabelPassStats stats{};
    auto* canvas = reinterpret_cast<RC::Unreal::UObject*>(canvasPtr);
    // Only style exact bowl-name matches within the map's dedicated canvas.
    if (g_textStyle == TextStyle::WhitePlate) {
        const auto prevCanvas = g_plateCanvas.exchange(canvasPtr, std::memory_order_relaxed);
        if (prevCanvas != canvasPtr) {
            std::lock_guard<std::mutex> lock(g_plateMutex);
            g_platedTextBlocks.clear();
        }
    }
    restyle_label_widget(canvas, 0, stats, bowlNames);
    // Replacing CanvasPanel children changes its indexed child list.
    // Process the collected bowl TextBlocks only after traversal is complete.
    for (auto* label : stats.pendingBoxLabels) {
        const auto outcome = apply_boxed_map_text(label);
        if (outcome.success) {
            ++stats.styled;
            if (outcome.method == "content_wrapped") ++stats.boxesNested;
            else if (outcome.method == "canvas_wrapped") ++stats.boxesDirect;
            else if (outcome.method == "existing_border") ++stats.boxesExisting;
        } else {
            ++stats.boxFailures;
            if (g_labelPasses.load(std::memory_order_relaxed) < 3)
                write_status("Box creation failed: reason=" + outcome.failure +
                             ", ancestor_depth=" + std::to_string(outcome.canvasDepth));
        }
    }
    const auto pass = g_labelPasses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pass <= 8) {
        write_status("Map label pass " + std::to_string(pass) +
                     " (" + stage + "): widgets=" + std::to_string(stats.visited) +
                     ", text_blocks=" + std::to_string(stats.textBlocks) +
                     ", bowl_name_matches=" + std::to_string(stats.bowlMatches) +
                     ", styled=" + std::to_string(stats.styled) +
                     ", box_failures=" + std::to_string(stats.boxFailures) +
                     ", nested_boxes=" + std::to_string(stats.boxesNested) +
                     ", direct_boxes=" + std::to_string(stats.boxesDirect) +
                     ", existing_borders=" + std::to_string(stats.boxesExisting) +
                     ", empty_text=" + std::to_string(stats.noName) +
                     ", panels=" + std::to_string(stats.panelWidgets) +
                     ", user_widgets=" + std::to_string(stats.userWidgets) +
                     ", rich_text_blocks=" + std::to_string(stats.richTextBlocks));
    }
    return;
}

// Collect live widgets using reflected UMG child APIs and valid widget-tree
// roots. Never treat a raw native placement-record handle as a widget pointer.
void collect_canvas_widget_addresses(RC::Unreal::UObject* widget, int depth,
                  std::unordered_set<std::uintptr_t>& addresses,
                  std::unordered_map<std::uint32_t, std::uintptr_t>& objectIndexes) {
    if (!widget || depth > 10 || addresses.size() >= 2048) return;
    const auto widgetAddress = reinterpret_cast<std::uintptr_t>(widget);
    if (!addresses.insert(widgetAddress).second) return;
    // The widget has already been obtained through UMG's reflected API.
    // Never use a native placement handle as a pointer here.
    std::int32_t internalIndex = -1;
    if (read_at(widgetAddress + 0xC, internalIndex) &&
        internalIndex > 0 && internalIndex < 4000000) {
        auto [it, inserted] = objectIndexes.emplace(
            static_cast<std::uint32_t>(internalIndex), widgetAddress);
        if (!inserted && it->second != widgetAddress) it->second = 0;
    }
    if (g_panelWidgetClass && g_getChildrenCountFn && g_getChildAtFn &&
        widget->IsA(g_panelWidgetClass)) {
        std::int32_t n = 0;
        widget->ProcessEvent(g_getChildrenCountFn, &n);
        if (n >= 0 && n <= 600) {
            for (int i = 0; i < n && addresses.size() < 2048; ++i) {
                GetPanelChildParams child{i, 0, nullptr};
                widget->ProcessEvent(g_getChildAtFn, &child);
                if (child.result && child.result != widget)
                    collect_canvas_widget_addresses(child.result, depth + 1, addresses, objectIndexes);
            }
        }
    } else if (g_userWidgetClass && widget->IsA(g_userWidgetClass)) {
        std::uintptr_t tree = 0, root = 0;
        if (read_at(reinterpret_cast<std::uintptr_t>(widget) + 0x2C0, tree) && tree &&
            read_at(tree + 0x30, root) && root &&
            root != reinterpret_cast<std::uintptr_t>(widget)) {
            collect_canvas_widget_addresses(reinterpret_cast<RC::Unreal::UObject*>(root),
                                            depth + 1, addresses, objectIndexes);
        }
    }
}

// Default native-ID styling pass. The only pointers on which we invoke UMG methods
// come from reflected LabelCanvas traversal and from the existing game's
// attachment callback. Native 104-byte records supply only numeric weak
// handles (InternalIndex in word0, reference serial/generation in word1).
// Do not resolve/dereference raw handles or retain object indexes across calls.
void apply_id_label_style_pass(std::uintptr_t mapWidget,
                               std::uintptr_t sourceContext) {
    if (!g_idLabelStyleEnabled || !mapWidget || !sourceContext) return;
    const auto pass = g_idLabelStylePasses.fetch_add(1,
        std::memory_order_relaxed) + 1;
    ArrayHeader geometry{}, labelSources{}, placed{};
    const bool labelSourcesOk = read_at(sourceContext + 0x80, labelSources) &&
        labelSources.num >= 0 && labelSources.num <= 1024 &&
        labelSources.max >= labelSources.num &&
        (labelSources.num == 0 || labelSources.data != 0);
    const bool geometryOk = read_at(sourceContext + 0x70, geometry) &&
        geometry.data && geometry.num > 0 && geometry.num <= kMaxSourceRecords &&
        geometry.max >= geometry.num;
    const bool placedOk = read_at(mapWidget + 0x530, placed) &&
        placed.data && placed.num >= 0 && placed.num <= 1024 &&
        placed.max >= placed.num;
    if (!geometryOk || !labelSourcesOk || !placedOk || !g_textBlockClass ||
        !g_getChildrenCountFn || !g_getChildAtFn || !g_panelWidgetClass) {
        if (pass <= 8) write_status("IDSTYLE skipped: missing geometry, native placement, or UMG reflection");
        return;
    }
    std::uintptr_t canvasAddress = 0;
    if (!read_at(mapWidget + 0x518, canvasAddress) || !canvasAddress) return;
    auto* canvas = reinterpret_cast<RC::Unreal::UObject*>(canvasAddress);
    if (!canvas->IsA(g_panelWidgetClass)) return;

    // Native entries are input metadata ONLY: never interpret +0 as a pointer.
    struct Candidate { int sourceIndex; bool bowl; bool ambiguous; };
    std::unordered_map<std::uint32_t, Candidate> byIndex;
    int unreadable = 0, category1 = 0, bowlCandidates = 0, indexCollisions = 0;
    int unsupportedNativeKeys = 0;
    for (int i = 0; i < placed.num; ++i) {
        std::array<std::byte, 104> record{};
        if (!safe_copy(record.data(),
                reinterpret_cast<const void*>(placed.data +
                    static_cast<std::uintptr_t>(i) * record.size()),
                record.size())) { ++unreadable; continue; }
        std::uint64_t weakHandle = 0, key = 0;
        std::memcpy(&weakHandle, record.data(), sizeof(weakHandle));
        std::memcpy(&key, record.data() + 0x30, sizeof(key));
        const auto category = static_cast<std::uint8_t>(key);
        const int sourceIndex = static_cast<std::int32_t>(key >> 32);
        // Unknown native categories or out-of-range indexes invalidate the
        // placement-layout assumption: never style from a partial table.
        if (!((category == 1 && sourceIndex >= 0 && sourceIndex < geometry.num) ||
              (category == 2 && sourceIndex >= 0 && sourceIndex < labelSources.num))) {
            ++unsupportedNativeKeys;
            continue;
        }
        const bool isBowl = category == 1 && sourceIndex >= 0 &&
            sourceIndex < geometry.num && index_is_replaced_bowl(sourceIndex);
        if (category == 1) ++category1;
        if (isBowl) ++bowlCandidates;
        const auto objectIndex = static_cast<std::uint32_t>(weakHandle);
        const auto serial = static_cast<std::uint32_t>(weakHandle >> 32);
        if (!objectIndex || !serial) continue;
        auto [it, inserted] = byIndex.emplace(objectIndex,
            Candidate{sourceIndex, isBowl, false});
        if (!inserted) { it->second.ambiguous = true; ++indexCollisions; }
    }

    if (unreadable != 0 || unsupportedNativeKeys != 0) {
        if (pass <= 8)
            write_status("LAYOUT native label-placement check failed: unreadable=" +
                std::to_string(unreadable) + ", unsupported_keys=" +
                std::to_string(unsupportedNativeKeys) +
                "; leaving native labels untouched.");
        return;
    }

    // Only style widgets independently identified by reflected UMG traversal
    // and the native attachment callback. Do not interpret packed UObject
    // registry entries as direct widget pointers.
    std::unordered_set<std::uintptr_t> canvasAddresses;
    std::unordered_map<std::uint32_t, std::uintptr_t> liveByIndex;
    collect_canvas_widget_addresses(canvas, 0, canvasAddresses, liveByIndex);
    std::unordered_map<std::uint32_t, std::uintptr_t> attachedByIndex;
    {
        std::lock_guard<std::mutex> lock(g_mapAttachmentMutex);
        attachedByIndex = g_mapAttachmentByObjectIndex;
    }
    std::unordered_set<std::uintptr_t> roots;
    int skippedAmbiguous = 0, absentTree = 0, absentAttachment = 0, mismatch = 0;
    for (const auto& [index, candidate] : byIndex) {
        if (!candidate.bowl) continue;
        if (candidate.ambiguous) { ++skippedAmbiguous; continue; }
        const auto foundTree = liveByIndex.find(index);
        if (foundTree == liveByIndex.end() || !foundTree->second) {
            ++absentTree; continue;
        }
        const auto foundAttachment = attachedByIndex.find(index);
        if (foundAttachment == attachedByIndex.end() || !foundAttachment->second) {
            ++absentAttachment; continue;
        }
        if (foundTree->second != foundAttachment->second ||
            foundTree->second == canvasAddress ||
            !canvasAddresses.count(foundTree->second)) {
            ++mismatch; continue;
        }
        // The two independently obtained live pointers agree.
        // Native weak handles are never dereferenced, and object indexes
        // are not retained after this rebuild.
        roots.insert(foundTree->second);
    }

    // Label roots may be TextBlocks or containers such as SizeBoxes. Style
    // their contained TextBlocks, never unrelated LabelCanvas descendants.
    LabelPassStats stats{};
    const std::unordered_set<std::wstring> unusedNames;
    if (g_textStyle == TextStyle::WhitePlate) {
        const auto previousCanvas = g_plateCanvas.exchange(canvasAddress,
            std::memory_order_relaxed);
        if (previousCanvas != canvasAddress) {
            std::lock_guard<std::mutex> lock(g_plateMutex);
            g_platedTextBlocks.clear();
        }
    }
    for (const auto root : roots) {
        auto* widget = reinterpret_cast<RC::Unreal::UObject*>(root);
        restyle_label_widget(widget, 0, stats, unusedNames, true);
    }
    // Defer changes to the canvas hierarchy until traversal has finished.
    for (auto* label : stats.pendingBoxLabels) {
        const auto outcome = apply_boxed_map_text(label);
        if (outcome.success) {
            ++stats.styled;
            if (outcome.method == "content_wrapped") ++stats.boxesNested;
            else if (outcome.method == "canvas_wrapped") ++stats.boxesDirect;
            else if (outcome.method == "existing_border") ++stats.boxesExisting;
        } else {
            ++stats.boxFailures;
            if (pass <= 3)
                write_status("IDSTYLE box failed: reason=" + outcome.failure);
        }
    }
    if (pass <= 8) {
        write_status("IDSTYLE pass=" + std::to_string(pass) +
            " placed=" + std::to_string(placed.num) +
            " category1=" + std::to_string(category1) +
            " bowl_candidates=" + std::to_string(bowlCandidates) +
            " eligible_roots=" + std::to_string(roots.size()) +
            " matched_textblocks=" + std::to_string(stats.bowlMatches) +
            " styled=" + std::to_string(stats.styled) +
            " boxes_nested=" + std::to_string(stats.boxesNested) +
            " boxes_direct=" + std::to_string(stats.boxesDirect) +
            " boxes_existing=" + std::to_string(stats.boxesExisting) +
            " box_failures=" + std::to_string(stats.boxFailures) +
            " skipped_ambiguous=" + std::to_string(skippedAmbiguous) +
            " absent_tree=" + std::to_string(absentTree) +
            " absent_attachment=" + std::to_string(absentAttachment) +
            " mismatched_addresses=" + std::to_string(mismatch) +
            " duplicated_native_indexes=" + std::to_string(indexCollisions) +
            " unreadable=" + std::to_string(unreadable));
    }
}

// Run the final label styling pass after native rebuild has completed.
// The native function returns double and takes three pointer-sized arguments.
double __fastcall hooked_full_map_rebuild(std::uintptr_t mapWidget,
                                          std::uintptr_t a2,
                                          std::uintptr_t a3) {
    if (g_idLabelStyleEnabled) {
        std::lock_guard<std::mutex> lock(g_mapAttachmentMutex);
        g_mapAttachmentByObjectIndex.clear();
    }
    g_captureMapAttachments.store(g_idLabelStyleEnabled, std::memory_order_release);
    const double result = g_originalFullMapRebuild(mapWidget, a2, a3);
    g_captureMapAttachments.store(false, std::memory_order_release);
    if (g_idLabelStyleEnabled) {
        apply_id_label_style_pass(mapWidget, a2);
        // No stale widget pointers/object indexes should survive a rebuild.
        std::lock_guard<std::mutex> lock(g_mapAttachmentMutex);
        g_mapAttachmentByObjectIndex.clear();
    } else {
        apply_map_label_style_pass(mapWidget, "rebuild-end");
    }
    return result;
}

// The StrokeLayer's circle markers come from its native-owned stroke records.
// Change only Num fields, AFTER map labels have been computed from the intact
// generator output. Preserve Data/Max, record count and source indexes so the
// game can still free all allocations normally and other trails remain intact.
int suppress_hidden_bowl_strokes(
    std::uintptr_t installedStrokeArray,
    const std::array<std::uint64_t, 8>& replacedIndices) {
    if (!installedStrokeArray) return -1;
    ArrayHeader strokes{};
    if (!read_at(installedStrokeArray, strokes) || !strokes.data || strokes.num < 0 ||
        strokes.num > 16384 || strokes.max < strokes.num) return -1;
    int suppressed = 0;
    constexpr std::int32_t zero = 0;
    for (int i = 0; i < strokes.num; ++i) {
        const auto record = strokes.data + static_cast<std::uintptr_t>(i) * 72;
        std::int32_t sourceIndex = -1;
        ArrayHeader points{}, flags{};
        if (!read_at(record + 0x38, sourceIndex) ||
            !read_at(record, points) || !read_at(record + 0x10, flags))
            return -1;
        if (sourceIndex < 0 || sourceIndex >= kMaxSourceRecords ||
            !(replacedIndices[static_cast<std::size_t>(sourceIndex) / 64] &
              (1ULL << (static_cast<std::size_t>(sourceIndex) % 64)))) continue;
        if (points.num < 0 || points.num > 4096 || points.max < points.num ||
            flags.num < 0 || flags.num > 4096 || flags.max < flags.num)
            return -1;
        if (!safe_copy(reinterpret_cast<void*>(record + 8), &zero, sizeof(zero)) ||
            !safe_copy(reinterpret_cast<void*>(record + 0x18), &zero, sizeof(zero)))
            return -1;
        ++suppressed;
    }
    return suppressed;
}

bool plausible_geometry(const std::vector<Vector3D>& p) {
    if (p.size() < 8 || p.size() > kMaxCenterlineBowlPoints) return false;
    for (const auto& q : p) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
        if (std::abs(q.x) > 1e9 || std::abs(q.y) > 1e9 || std::abs(q.z) > 1e9) return false;
    }
    return true;
}

// For each elevation slice through the perimeter, intersect the 3-D boundary
// edges with that Z level, then take the midpoint of the widest intersecting
// pair. This produces descending centreline vertices, without modifying world
// trails or needing a navmesh. It is an approximation for highly concave bowls.
std::vector<Vector3D> slice_centerline(const std::vector<Vector3D>& ring) {
    std::vector<Vector3D> result;
    if (!plausible_geometry(ring)) return result;
    double minZ = ring.front().z, maxZ = minZ;
    for (const auto& p : ring) {
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }
    const double relief = maxZ - minZ;
    if (relief < 200.0 || !std::isfinite(relief)) return result;

    struct Cross { double x, y; };
    for (int s = 0; s < kCenterlineSlices; ++s) {
        // Avoid apex/end degeneracy, so two boundary intersections are available.
        const double fraction = 0.02 + 0.96 * static_cast<double>(s) / (kCenterlineSlices - 1);
        const double elevation = maxZ - relief * fraction;
        std::vector<Cross> intersections;
        intersections.reserve(ring.size());
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % ring.size()];
            const double dz = b.z - a.z;
            if (std::abs(dz) < 1e-9) continue;
            const double t = (elevation - a.z) / dz;
            if (t >= 0.0 && t < 1.0) {
                intersections.push_back({a.x + t * (b.x - a.x),
                                         a.y + t * (b.y - a.y)});
            }
        }
        if (intersections.size() < 2) continue;
        double widest = -1.0;
        Cross center{};
        for (std::size_t i = 0; i < intersections.size(); ++i) {
            for (std::size_t j = i + 1; j < intersections.size(); ++j) {
                const double dx = intersections[i].x - intersections[j].x;
                const double dy = intersections[i].y - intersections[j].y;
                const double d2 = dx * dx + dy * dy;
                if (d2 > widest) {
                    widest = d2;
                    center = {(intersections[i].x + intersections[j].x) * 0.5,
                              (intersections[i].y + intersections[j].y) * 0.5};
                }
            }
        }
        if (widest < 1.0) continue;
        result.push_back({center.x, center.y, elevation});
    }
    if (result.size() < 8) return {};
    // Softly smooth XY, preserving strict top-to-bottom elevation order.
    std::vector<Vector3D> smooth = result;
    for (std::size_t i = 1; i + 1 < result.size(); ++i) {
        smooth[i].x = (result[i-1].x + 2 * result[i].x + result[i+1].x) * 0.25;
        smooth[i].y = (result[i-1].y + 2 * result[i].y + result[i+1].y) * 0.25;
    }
    // Reject impossible output rather than forwarding it to native renderer.
    for (const auto& p : smooth) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return {};
    }
    return smooth;
}

// Check the *live* geometry-record fields before changing any data. A wrong
// table stride usually produces many invalid TArray headers or bool flags.
// This is intentionally tolerant of empty records and unknown shape types;
// individual bowls are still checked with plausible_geometry().
bool plausible_source_record_table(const std::vector<std::byte>& records,
                                   std::int32_t count, int& malformed) {
    malformed = 0;
    if (count < 1 || count > kMaxSourceRecords ||
        records.size() != static_cast<std::size_t>(count) * kSourceRecordBytes)
        return false;
    for (int i = 0; i < count; ++i) {
        const auto* record = records.data() + static_cast<std::size_t>(i) * kSourceRecordBytes;
        ArrayHeader points{};
        std::uint8_t closed = 0;
        std::memcpy(&points, record + 24, sizeof(points));
        std::memcpy(&closed, record + 40, sizeof(closed));
        if (closed > 1 || points.num < 0 || points.max < points.num ||
            points.max > 1000000 ||
            (points.num > 0 && (points.data < 0x10000 || (points.data & 7) != 0)))
            ++malformed;
    }
    return malformed <= std::max(3, count / 4);
}

// Every invocation is synchronous. Build private shallow copies of the source
// context + 120-byte record table, with owned centreline arrays. Never modify
// the game's actual input array, world geometry, or persistent resort data.
unsigned char __fastcall hooked_generator(std::uintptr_t mapWidget,
                                          std::uintptr_t sourceContext,
                                          unsigned char rebuildMode,
                                          std::uintptr_t output) {
    // This is the native map's dedicated LabelCanvas, not a global UMG panel.
    std::uintptr_t labelCanvas = 0;
    if (mapWidget && read_at(mapWidget + 0x518, labelCanvas) && labelCanvas)
        g_activeLabelCanvas.store(labelCanvas, std::memory_order_release);
    // Native ID mode does not need per-record FText name extraction.
    if (!g_idLabelStyleEnabled) refresh_bowl_names(sourceContext);
    if (!sourceContext) {
        publish_bowl_indices({});
        return g_original(mapWidget, sourceContext, rebuildMode, output);
    }

    ArrayHeader originals{};
    if (!read_at(sourceContext + 112, originals) || !originals.data ||
        originals.num < 5 || originals.num > kMaxSourceRecords ||
        originals.max < originals.num) {
        publish_bowl_indices({});
        return g_original(mapWidget, sourceContext, rebuildMode, output);
    }
    alignas(16) std::array<std::byte, kSourceContextBytes> context{};
    std::vector<std::byte> records(static_cast<std::size_t>(originals.num) * kSourceRecordBytes);
    if (!safe_copy(context.data(), reinterpret_cast<const void*>(sourceContext), context.size()) ||
        !safe_copy(records.data(), reinterpret_cast<const void*>(originals.data), records.size())) {
        publish_bowl_indices({});
        return g_original(mapWidget, sourceContext, rebuildMode, output);
    }

    int malformedSourceRecords = 0;
    if (!plausible_source_record_table(records, originals.num, malformedSourceRecords)) {
        publish_bowl_indices({});
        write_status("LAYOUT runtime source-record check failed: malformed=" +
                     std::to_string(malformedSourceRecords) +
                     " of " + std::to_string(originals.num) +
                     "; passing map through unchanged.");
        return g_original(mapWidget, sourceContext, rebuildMode, output);
    }
    std::vector<std::vector<Vector3D>> paths(static_cast<std::size_t>(originals.num));
    int bowlCount = 0, replaced = 0;
    std::array<std::uint64_t, 8> replacedIndices{};
    for (int i = 0; i < originals.num; ++i) {
        auto* record = records.data() + static_cast<std::size_t>(i) * kSourceRecordBytes;
        std::uint8_t shape = 0, closed = 0;
        ArrayHeader points{};
        std::memcpy(&shape, record, 1);
        std::memcpy(&points, record + 24, sizeof(points));
        std::memcpy(&closed, record + 40, 1);
        // Closed multi-point records represent bowls; two-point shape-1
        // records represent lift paths in the supported native layout.
        if (shape != 0 || closed != 1 || points.num < 8 ||
            points.num > kMaxCenterlineBowlPoints || points.max < points.num ||
            !points.data) continue;
        ++bowlCount;
        std::vector<Vector3D> ring(static_cast<std::size_t>(points.num));
        if (!safe_copy(ring.data(), reinterpret_cast<const void*>(points.data),
                       ring.size() * sizeof(Vector3D)) || !plausible_geometry(ring)) continue;

        auto& path = paths[static_cast<std::size_t>(i)];
        path = slice_centerline(ring);
        if (path.size() < 2 || path.size() > 512) continue;

        const std::uintptr_t newData = reinterpret_cast<std::uintptr_t>(path.data());
        const auto newCount = static_cast<std::int32_t>(path.size());
        const std::uint8_t open = 0;
        std::memcpy(record + 24, &newData, sizeof(newData));
        std::memcpy(record + 32, &newCount, sizeof(newCount));
        std::memcpy(record + 36, &newCount, sizeof(newCount));
        std::memcpy(record + 40, &open, sizeof(open));
        ++replaced;
        replacedIndices[static_cast<std::size_t>(i) / 64] |=
            1ULL << (static_cast<std::size_t>(i) % 64);
    }

    auto patched = originals;
    patched.data = reinterpret_cast<std::uintptr_t>(records.data());
    patched.max = originals.num;
    std::memcpy(context.data() + 112, &patched, sizeof(patched));
    const auto call = g_mapCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 12) {
        write_status("Map call " + std::to_string(call) +
                     ": closed_bowls=" + std::to_string(bowlCount) +
                     ", replaced=" + std::to_string(replaced) +
                     ", view_mode=" + std::to_string(static_cast<int>(rebuildMode)));
    }
    // The original function consumes these arrays synchronously and generates
    // the game's own map stroke objects. All temporary storage is still alive.
    const auto result = g_original(mapWidget,
        reinterpret_cast<std::uintptr_t>(context.data()), rebuildMode, output);
    if (result != 0) {
        // CRITICAL: do not blank point arrays here. The parent map full-rebuild
        // function still reads them to place trail-name labels. Hidden-mode
        // suppression is deferred to hooked_install_map_strokes instead.
        publish_bowl_indices(replacedIndices);
        std::uintptr_t layer = 0;
        if (read_at(mapWidget + 0x520, layer) && layer)
            g_mapStrokeLayer.store(layer, std::memory_order_release);
    } else {
        publish_bowl_indices({});
    }
    return result;
}

} // namespace

class ResortMapBowlTweaks final : public RC::CppUserModBase {
public:
    ResortMapBowlTweaks() : RC::CppUserModBase() {
        ModName = STR("ResortMapBowlTweaks");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Configurable bowl centrelines and bowl-only map labels");
        ModAuthors = STR("Resort Map Bowl Tweaks project");
        // Do not touch logs here: INI settings are loaded in on_unreal_init().
        // Logging is strictly opt-in via debug=true in the mod-root INI.
    }

    void on_unreal_init() override {
        if (m_hooked) return;
        load_mod_settings();
        // Resolve all five native entrypoints before modifying any game code.
        if (!resolve_native_functions(g_nativeFunctions)) return;
        const std::uintptr_t address = g_nativeFunctions.generator;
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            write_status("ERROR: MH_Initialize failed: " + std::to_string(status));
            return;
        }
        m_minhookReady = true;
        g_hookAddress = reinterpret_cast<void*>(address);
        status = MH_CreateHook(g_hookAddress,
                               reinterpret_cast<void*>(&hooked_generator),
                               reinterpret_cast<void**>(&g_original));
        if (status != MH_OK) {
            write_status("ERROR: MH_CreateHook failed: " + std::to_string(status));
            return;
        }
        m_hookCreated = true;
        status = MH_EnableHook(g_hookAddress);
        if (status != MH_OK) {
            write_status("ERROR: MH_EnableHook failed: " + std::to_string(status));
            return;
        }
        m_hooked = true;
        write_status("Map geometry hook enabled.");
        if (g_strokeStyle != StrokeStyle::Solid) {
            g_paintLineAddress = reinterpret_cast<void*>(g_nativeFunctions.paintLine);
            if (!is_executable_address(g_paintLineAddress)) {
                write_status("WARNING: Map paint-line function invalid; bowl lines may remain solid.");
            } else {
                status = MH_CreateHook(g_paintLineAddress,
                            reinterpret_cast<void*>(&hooked_paint_line),
                            reinterpret_cast<void**>(&g_originalPaintLine));
                if (status != MH_OK) {
                    write_status("WARNING: Paint-line hook create failed (" +
                                 std::to_string(status) + "); bowl lines may remain solid.");
                } else {
                    m_paintHookCreated = true;
                    status = MH_EnableHook(g_paintLineAddress);
                    if (status == MH_OK) {
                        m_paintHooked = true;
                        write_status(std::string("Bowl paint-line hook enabled: ") + line_style_name(g_strokeStyle));
                    } else {
                        write_status("WARNING: Paint-line hook enable failed (" +
                                     std::to_string(status) + "); bowl lines may remain solid.");
                    }
                }
            }
        }
        { // All three text modes explicitly style bowl labels.
            g_textBlockClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/UMG.TextBlock"));
            g_panelWidgetClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/UMG.PanelWidget"));
            g_userWidgetClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/UMG.UserWidget"));
            g_richTextBlockClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/UMG.RichTextBlock"));
            if (g_textStyle == TextStyle::WhitePlate) {
                g_canvasPanelClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                    nullptr, nullptr, STR("/Script/UMG.CanvasPanel"));
                g_canvasSlotClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                    nullptr, nullptr, STR("/Script/UMG.CanvasPanelSlot"));
                g_borderClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                    nullptr, nullptr, STR("/Script/UMG.Border"));
                g_contentWidgetClass = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                    nullptr, nullptr, STR("/Script/UMG.ContentWidget"));
                const auto func = [](const wchar_t* name) {
                    return RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                        nullptr, nullptr, name);
                };
                g_setTextFn = func(L"/Script/UMG.TextBlock:SetText");
                g_getParentFn = func(L"/Script/UMG.Widget:GetParent");
                g_setVisibilityFn = func(L"/Script/UMG.Widget:SetVisibility");
                g_canvasAddChildFn = func(L"/Script/UMG.CanvasPanel:AddChildToCanvas");
                g_borderSetContentFn = func(L"/Script/UMG.ContentWidget:SetContent");
                g_borderSetBrushFn = func(L"/Script/UMG.Border:SetBrush");
                g_borderSetPaddingFn = func(L"/Script/UMG.Border:SetPadding");
                g_contentSetContentFn = func(L"/Script/UMG.ContentWidget:SetContent");
                g_panelRemoveChildFn = func(L"/Script/UMG.PanelWidget:RemoveChild");
                g_canvasSlotSetLayoutFn = func(L"/Script/UMG.CanvasPanelSlot:SetLayout");
                g_canvasSlotSetAutoSizeFn = func(L"/Script/UMG.CanvasPanelSlot:SetAutoSize");
                g_canvasSlotSetZOrderFn = func(L"/Script/UMG.CanvasPanelSlot:SetZOrder");
                write_status("Boxed-label reflection: Canvas=" +
                    std::to_string(g_canvasPanelClass != nullptr) +
                    " Slot=" + std::to_string(g_canvasSlotClass != nullptr) +
                    " Border=" + std::to_string(g_borderClass != nullptr) +
                    " SetText=" + std::to_string(g_setTextFn != nullptr) +
                    " SetBrush=" + std::to_string(g_borderSetBrushFn != nullptr) +
                    " AddChild=" + std::to_string(g_canvasAddChildFn != nullptr) +
                    " ContentWidget=" + std::to_string(g_contentWidgetClass != nullptr) +
                    " ContentSet=" + std::to_string(g_contentSetContentFn != nullptr) +
                    " PanelRemove=" + std::to_string(g_panelRemoveChildFn != nullptr));
            }
            g_setFontFn = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/UMG.TextBlock:SetFont"));
            g_setColorFn = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/UMG.TextBlock:SetColorAndOpacity"));
            g_getChildrenCountFn = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/UMG.PanelWidget:GetChildrenCount"));
            g_getChildAtFn = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/UMG.PanelWidget:GetChildAt"));
            if (!g_textBlockClass || !g_panelWidgetClass || !g_setFontFn ||
                !g_setColorFn || !g_getChildrenCountFn || !g_getChildAtFn) {
                write_status("WARNING: Required UMG reflection not found; label text may remain default.");
                write_status("  TextBlock=" + std::to_string(g_textBlockClass != nullptr) +
                             " PanelWidget=" + std::to_string(g_panelWidgetClass != nullptr) +
                             " SetFont=" + std::to_string(g_setFontFn != nullptr) +
                             " SetColor=" + std::to_string(g_setColorFn != nullptr) +
                             " Count=" + std::to_string(g_getChildrenCountFn != nullptr) +
                             " GetChildAt=" + std::to_string(g_getChildAtFn != nullptr));
            }
        }
        // Needed for hidden-mode marker suppression EVEN when text=default.
        // This function runs after the label builder has consumed the original
        // projected data, so it is safe to blank bowl stroke points here.
        if (g_strokeStyle == StrokeStyle::Hidden || g_textBlockClass) {
            g_installMapStrokesAddress = reinterpret_cast<void*>(g_nativeFunctions.installMapStrokes);
            if (!is_executable_address(g_installMapStrokesAddress)) {
                write_status("WARNING: Map stroke installer not executable; hidden markers may remain visible.");
            } else {
                status = MH_CreateHook(g_installMapStrokesAddress,
                    reinterpret_cast<void*>(&hooked_install_map_strokes),
                    reinterpret_cast<void**>(&g_originalInstallMapStrokes));
                if (status != MH_OK) {
                    write_status("WARNING: Map stroke installer hook creation failed: " + std::to_string(status));
                } else {
                    m_labelHookCreated = true;
                    status = MH_EnableHook(g_installMapStrokesAddress);
                    if (status == MH_OK) {
                        m_labelHooked = true;
                        write_status("Post-label map stroke installation hook enabled.");
                    } else write_status("WARNING: Map stroke installer hook enable failed: " + std::to_string(status));
                }
            }
        }
        // The attachment hook serves both identification modes: native-ID
        // captures widget identities; name-based mode primes text styling.
        if (g_textBlockClass && g_setFontFn && g_setColorFn) {
            g_addMapLabelAddress = reinterpret_cast<void*>(g_nativeFunctions.addMapLabel);
            if (is_executable_address(g_addMapLabelAddress)) {
                status = MH_CreateHook(g_addMapLabelAddress,
                    reinterpret_cast<void*>(&hooked_add_map_label),
                    reinterpret_cast<void**>(&g_originalAddMapLabel));
                if (status == MH_OK) {
                    m_addLabelHookCreated = true;
                    status = MH_EnableHook(g_addMapLabelAddress);
                    if (status == MH_OK) {
                        m_addLabelHooked = true;
                        write_status(g_idLabelStyleEnabled
                            ? "Map-label attachment capture enabled (ID default)."
                            : "Pre-attachment name-style hook enabled.");
                    } else write_status("WARNING: Pre-attachment label hook enable failed: " +
                                        std::to_string(status));
                } else write_status("WARNING: Pre-attachment label hook create failed: " +
                                    std::to_string(status));
            } else write_status("WARNING: Label attachment function is not executable.");
        }
        // Rebuild-end styling hook is optional; failure to install it does not
        // prevent the stroke-install hook from operating.
        if (g_textBlockClass && g_setFontFn && g_setColorFn) {
            g_fullMapRebuildAddress = reinterpret_cast<void*>(g_nativeFunctions.fullMapRebuild);
            if (is_executable_address(g_fullMapRebuildAddress)) {
                status = MH_CreateHook(g_fullMapRebuildAddress,
                    reinterpret_cast<void*>(&hooked_full_map_rebuild),
                    reinterpret_cast<void**>(&g_originalFullMapRebuild));
                if (status == MH_OK) {
                    m_rebuildHookCreated = true;
                    status = MH_EnableHook(g_fullMapRebuildAddress);
                    if (status == MH_OK) {
                        m_rebuildHooked = true;
                        write_status("Rebuild-end label restyle hook enabled.");
                    } else write_status("WARNING: Rebuild-end hook enable failed: " +
                                        std::to_string(status));
                } else write_status("WARNING: Rebuild-end hook create failed: " +
                                    std::to_string(status));
            } else write_status("WARNING: Rebuild function is not executable.");
        }
    }

    ~ResortMapBowlTweaks() override {
        if (m_addLabelHooked) MH_DisableHook(g_addMapLabelAddress);
        if (m_addLabelHookCreated) MH_RemoveHook(g_addMapLabelAddress);
        if (m_rebuildHooked) MH_DisableHook(g_fullMapRebuildAddress);
        if (m_rebuildHookCreated) MH_RemoveHook(g_fullMapRebuildAddress);
        if (m_labelHooked) MH_DisableHook(g_installMapStrokesAddress);
        if (m_labelHookCreated) MH_RemoveHook(g_installMapStrokesAddress);
        if (m_paintHooked) MH_DisableHook(g_paintLineAddress);
        if (m_paintHookCreated) MH_RemoveHook(g_paintLineAddress);
        if (m_hooked) MH_DisableHook(g_hookAddress);
        if (m_hookCreated) MH_RemoveHook(g_hookAddress);
        if (m_minhookReady) MH_Uninitialize();
    }

private:
    bool m_addLabelHookCreated = false;
    bool m_addLabelHooked = false;
    bool m_rebuildHookCreated = false;
    bool m_rebuildHooked = false;
    bool m_labelHookCreated = false;
    bool m_labelHooked = false;
    bool m_paintHookCreated = false;
    bool m_paintHooked = false;
    bool m_minhookReady = false;
    bool m_hookCreated = false;
    bool m_hooked = false;
};

extern "C" {
    __declspec(dllexport) RC::CppUserModBase* start_mod() {
        return new ResortMapBowlTweaks();
    }
    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) {
        delete mod;
    }
}
