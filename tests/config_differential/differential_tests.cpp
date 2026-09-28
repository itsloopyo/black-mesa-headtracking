// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
//
// The differential test for the conversion from HeadTracking.ini to CameraUnlock.ini.
//
// Three readings of every input, and what may differ between them:
//
//   Oracle     the reader of the newest published build (v0.1.1, 1d2613d, core e6ce21d),
//              compiled from its own sources (oracle_api.h)
//   Import     the frozen reader in src/legacy_config/
//   Migration  the config owner's Load in a folder holding only the input as HeadTracking.ini,
//              which imports it through MakeLegacyImport into a new CameraUnlock.ini, then the
//              canonical reader and table on it
//
// Comparison 1, oracle against import, is what a player sees change that the conversion did
// not cause: commits since the published build that change how the file is read. There are
// none. src/config.cpp, config.h and hotkeys.h were unchanged from v0.1.0 to the commit the
// reader was frozen at, and every core source either reader compiles holds the same bytes at
// e6ce21d and at the pin, which SourcesAreThePinnedOnes checks.
//
// Comparison 2, import against migration, is the proof for the conversion. It allows the
// approved changes and normalisations core's data/config-format.json records that apply here,
// and nothing else:
//
//   pose_shaping  a sensitivity, inversion, deadzone or WorldScale the player set away from the
//                 shipped value is dropped, and the session runs at the shipped value, which the
//                 code now applies: identity, and kWorldUnitsPerMetre for WorldScale.
//
//   N3            a hotkey code for Ctrl, Shift or Alt on its own (0x10-0x12, 0xA0-0xA5), which
//                 the frozen reader accepts, is unbound and logged, and the action keeps its
//                 Ctrl+Shift chord.
//
//   N4            a [Position] limit above the canonical rows' 10 metres, which the frozen reader
//                 takes with no upper bound, imports as 10 and is logged, and the row is carried
//                 as the player's.
//
// The reader replaces a float that is not finite, clamps the smoothing pair into 0 to 1, and
// keeps every hotkey code inside 0x01-0xFE, so neither N1 nor N2 can apply.
//
// LightFollowsHead and LightMultiplier are new rows. v0.1.1 always turned the flashlight with
// the head at core's kDefaultLightMultiplier, which is what both rows default to, so every input
// starts with the light as it did.
//
// A row the player never changed from what v0.1.1 shipped follows Defaults.ini: the import lists
// it in follows_defaults_ini and the migration writes it `default`, the tracking mode pair as one
// unit. The test derives that list from what the session ran on, a row whose every observed value
// is the one v0.1.1 ran with no file, and holds the import's list to it on every input. The
// first-run file, the empty file and no file list every row and migrate to the committed file
// byte for byte.
//
// Each input with a file migrates three times: over a Defaults.ini the owner creates with the
// built-in values, from a read-only HeadTracking.ini, and over a Defaults.ini that differs from
// the built-in value on every global row. Over the first two the session runs as the import
// read, since v0.1.1's defaults are the built-in values. Over the third a row the player never
// changed is `default` and takes Defaults.ini's value, and a changed row keeps the player's.
//
// Inputs: the published build's first-run file (no build shipped or seeded a config, so every
// player's file started as that one), no file, an empty file, core's corpus of mutations of the
// first-run file, and the first-run file with each hotkey set to each code from 0x01 to 0xFE.

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "config.h"
#include "legacy_config/legacy_config.h"
#include "oracle_api.h"
#include "position_mapping.h"

#include "cameraunlock/config/canonical_ini.h"
#include "cameraunlock/config/config_owner.h"
#include "cameraunlock/config/legacy_import.h"
#include "cameraunlock/config/testing/ini_mutations.h"
#include "cameraunlock/effects/head_follow_light.h"
#include "cameraunlock/input/key_bindings.h"
#include "cameraunlock/tracking/tracking_mode.h"

namespace {

using cameraunlock::TrackingMode;
namespace cfg = cameraunlock::config;
using cfg::schema::Concept;
namespace fs = std::filesystem;
namespace testing = cameraunlock::config::testing;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %s\n", what.c_str());
}

// ---- Provenance ------------------------------------------------------------------------------
//
// Every source the oracle and the import compile, pinned by the SHA-256 of its bytes. The
// oracle's files are the published build's, taken with `git show v0.1.1:src/<file>`. The core
// files both readers compile are hash-equal to `git -C cameraunlock-core show e6ce21d:<path>`,
// the pin of both v0.1.0 and v0.1.1, so the readers differ only where the mod's own reader
// changed.
// The frozen import is pinned at the commit that froze it, so nothing edits it afterwards.

struct Pinned {
    const char* path;
    const char* sha256;
};

constexpr Pinned kPinned[] = {
    // The oracle: v0.1.1:src/...
    {"tests/config_differential/oracle/src/config.cpp", "5db63e56ac510e8f61368128796128fe10dd1dc1419272e54e972d4859f69ddd"},
    {"tests/config_differential/oracle/src/config.h", "1d76d8a303c6c94eb8cd412e94a63dcad909c4050333a1829fe45753a0bde971"},
    {"tests/config_differential/oracle/src/hotkeys.h", "18bec90b2abc673dd141e69ec252c2d552d96a9b35700aa20644a12435bfa30c"},
    {"tests/config_differential/oracle/src/debug_log.h", "93da8c6ce36715bc6f9327274927513998aef9c87c176e71617b89cc12fce5cb"},
    // Both readers: core at the pin, hash-equal to e6ce21d:cpp/...
    {"cameraunlock-core/cpp/include/cameraunlock/config/ini_reader.h", "a7ffb44210ff59672fa97e8e5feaa2cb3e81938fcc0334a384c68bc371b3857a"},
    {"cameraunlock-core/cpp/src/config/ini_reader.cpp", "e01515c2656aaf533bae4350dc45b702c3e3d4043935743dcc9bd5ea581fbe1c"},
    {"cameraunlock-core/cpp/include/cameraunlock/logging/file_log.h", "43bdd2ef8554c78e5f440333463750c13b95110fe672b0b6e273244df9e7d169"},
    {"cameraunlock-core/cpp/src/logging/file_log.cpp", "73c53c2baa06bbfebe8211f62678aa2b60cb95f604743d3686951ba56b87ea47"},
    // The oracle only: the headers v0.1.1's config.h and config.cpp include beside the reader.
    {"cameraunlock-core/cpp/include/cameraunlock/os/module_paths.h", "6d049431be9520bd07f4e3567e354d662c73e7ad44db47d00aae0f53a8233dea"},
    {"cameraunlock-core/cpp/include/cameraunlock/data/position_settings.h", "b24dceb8e25475aebc5a468a5c7362a4a4e64204d183d1408525345f32f547f5"},
    {"cameraunlock-core/cpp/include/cameraunlock/math/smoothing_utils.h", "fc2146f8c585e5f610c7234e302f59de4945679cfa28ff479ca47477ec073f22"},
    {"cameraunlock-core/cpp/include/cameraunlock/math/angle_utils.h", "d7a905270933e3cb0c4c361d29d3fd701655498cbcd1875ea79d180468bdbe6a"},
    // The import: src/debug_log.h is v0.1.1's, the legacy folder is frozen.
    {"src/debug_log.h", "93da8c6ce36715bc6f9327274927513998aef9c87c176e71617b89cc12fce5cb"},
    {"src/legacy_config/legacy_config.h", "01308ed0b96f3d3742cff5a6f163712dde3ed84a40fafd32057837f7b2142f19"},
    {"src/legacy_config/legacy_config.cpp", "4daf20e3cc3a2acd1b495879920b8345708abac1bf7b4a535ccc03eb3e872551"},
};

std::string ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Sha256Hex(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA256) failed");
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    const bool ok =
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                      static_cast<ULONG>(bytes.size()), 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) throw std::runtime_error("SHA-256 failed");
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

fs::path SourcePath(const std::string& relative) { return fs::path(BMS_SOURCE_DIR) / relative; }

void SourcesAreThePinnedOnes() {
    for (const Pinned& p : kPinned) {
        const std::string actual = Sha256Hex(ReadFileBytes(SourcePath(p.path)));
        if (actual != p.sha256) std::printf("  %s is %s\n", p.path, actual.c_str());
        Check(actual == p.sha256, std::string(p.path) + " holds the pinned bytes");
    }
}

// ---- Scratch folders -------------------------------------------------------------------------
//
// One folder per reading: GetPrivateProfileString, which both readers sit on, is free to cache
// the file it last read. `game` stands for the folder holding bms.exe; Defaults.ini sits in
// `global` beside it.

class Scratch {
public:
    Scratch() {
        static unsigned s_next = 0;
        wchar_t temp[MAX_PATH + 1] = {};
        if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
        root_ = fs::path(temp) / ("bms_ht_diff_" + std::to_string(GetCurrentProcessId()) + "_" +
                                  std::to_string(s_next++));
        Remove();
        fs::create_directories(root_ / "game");
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    // A scanner can still hold a file the test just wrote, and a destructor must not throw, so a
    // folder left behind is reported and the run carries on.
    ~Scratch() {
        try {
            Remove();
        } catch (const fs::filesystem_error& e) {
            std::printf("  scratch folder left behind: %s\n", e.what());
        }
    }

    std::string dir() const { return (root_ / "game").string(); }
    std::wstring wdir() const { return (root_ / "game").wstring(); }
    std::string ini() const { return dir() + "\\HeadTracking.ini"; }
    std::wstring wini() const { return wdir() + L"\\HeadTracking.ini"; }
    fs::path canonical() const { return root_ / "game" / "CameraUnlock.ini"; }
    fs::path defaults() const { return root_ / "global" / "Defaults.ini"; }

    void Write(const std::string& bytes) const {
        std::ofstream out(ini(), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("cannot write " + ini());
    }

    void WriteDefaults(const std::string& bytes) const {
        fs::create_directories(defaults().parent_path());
        std::ofstream out(defaults(), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("cannot write " + defaults().string());
    }

    // Every file in the game folder, by name, with its bytes.
    std::vector<std::pair<std::string, std::string>> Listing() const {
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto& entry : fs::directory_iterator(root_ / "game")) {
            files.push_back({entry.path().filename().string(), ReadFileBytes(entry.path())});
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::set<std::string> Names() const {
        std::set<std::string> names;
        for (const auto& entry : fs::directory_iterator(root_ / "game")) names.insert(entry.path().filename().string());
        return names;
    }

    cfg::ConfigOwnerOptions<headtracking::Config> Options() const {
        return headtracking::MakeConfigOwnerOptions(wdir() + L"\\", cfg::DefaultsFile::At(defaults().wstring()));
    }

private:
    void Remove() const {
        if (!fs::exists(root_)) return;
        for (const auto& entry : fs::recursive_directory_iterator(root_)) {
            SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        fs::remove_all(root_);
    }

    fs::path root_;
};

// ---- What a reading does ---------------------------------------------------------------------

std::uint32_t Bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

enum Action { kToggle, kCycleMode, kYawMode };

// One registered binding: the action, the virtual-key code, and the modifiers it needs (0, or
// Ctrl+Shift as cameraunlock::input::KeyModifiers spells it).
using Hotkey = std::tuple<int, int, unsigned>;
constexpr unsigned kPlain = 0;
constexpr unsigned kCtrlShift = 3;

// Everything a reading decides that the running mod acts on: the settings, the state at
// startup, what the pose pipeline is handed, and the bindings the poller registers.
struct Observed {
    int port = 0;
    bool start_enabled = false;
    bool start_world_yaw = false;
    int start_mode = 0;
    float sens_yaw = 0, sens_pitch = 0, sens_roll = 0;
    bool invert_yaw = false, invert_pitch = false, invert_roll = false;
    float deadzone_yaw = 0, deadzone_pitch = 0, deadzone_roll = 0;
    float local_smoothing = 0, remote_smoothing = 0;
    float pos_sens_x = 0, pos_sens_y = 0, pos_sens_z = 0;
    float limit_x = 0, limit_y = 0, limit_y_down = 0, limit_z = 0, limit_z_back = 0;
    // Metres to Source units per axis, carrying the inversion as its sign.
    float scale_x = 0, scale_y = 0, scale_z = 0;
    float fov = 0;
    bool log_to_file = false;
    bool light_follows_head = false;
    float light_multiplier = 0;
    std::vector<Hotkey> hotkeys;
};

std::vector<std::string> Differences(const Observed& a, const Observed& b) {
    std::vector<std::string> out;
    const auto flt = [&out](float x, float y, const char* what) {
        if (Bits(x) != Bits(y)) out.push_back(what);
    };
    if (a.port != b.port) out.push_back("UDP port");
    if (a.start_enabled != b.start_enabled) out.push_back("tracking on at startup");
    if (a.start_world_yaw != b.start_world_yaw) out.push_back("yaw mode at startup");
    if (a.start_mode != b.start_mode) out.push_back("tracking mode at startup");
    flt(a.sens_yaw, b.sens_yaw, "yaw sensitivity");
    flt(a.sens_pitch, b.sens_pitch, "pitch sensitivity");
    flt(a.sens_roll, b.sens_roll, "roll sensitivity");
    if (a.invert_yaw != b.invert_yaw) out.push_back("invert yaw");
    if (a.invert_pitch != b.invert_pitch) out.push_back("invert pitch");
    if (a.invert_roll != b.invert_roll) out.push_back("invert roll");
    flt(a.deadzone_yaw, b.deadzone_yaw, "yaw deadzone");
    flt(a.deadzone_pitch, b.deadzone_pitch, "pitch deadzone");
    flt(a.deadzone_roll, b.deadzone_roll, "roll deadzone");
    flt(a.local_smoothing, b.local_smoothing, "local smoothing");
    flt(a.remote_smoothing, b.remote_smoothing, "remote smoothing");
    flt(a.pos_sens_x, b.pos_sens_x, "position sensitivity x");
    flt(a.pos_sens_y, b.pos_sens_y, "position sensitivity y");
    flt(a.pos_sens_z, b.pos_sens_z, "position sensitivity z");
    flt(a.limit_x, b.limit_x, "limit x");
    flt(a.limit_y, b.limit_y, "limit y");
    flt(a.limit_y_down, b.limit_y_down, "limit y down");
    flt(a.limit_z, b.limit_z, "limit z");
    flt(a.limit_z_back, b.limit_z_back, "limit z back");
    flt(a.scale_x, b.scale_x, "position scale x");
    flt(a.scale_y, b.scale_y, "position scale y");
    flt(a.scale_z, b.scale_z, "position scale z");
    flt(a.fov, b.fov, "FOV override");
    if (a.log_to_file != b.log_to_file) out.push_back("log to file");
    if (a.light_follows_head != b.light_follows_head) out.push_back("light follows head");
    flt(a.light_multiplier, b.light_multiplier, "light multiplier");
    if (a.hotkeys != b.hotkeys) out.push_back("hotkeys");
    return out;
}

// Hand copied, not compiled from the published sources: the oracle library exports only the
// reader. v0.1.1:src/hotkey_handler.cpp:26-32 (HotkeyHandler::Start): the three configured
// codes NavGuarded, the Y, H and G chords ChordGuarded.
std::vector<Hotkey> LegacyHotkeys(int toggle_vk, int yaw_mode_vk, int mode_cycle_vk) {
    std::vector<Hotkey> keys = {
        {kToggle, toggle_vk, kPlain}, {kYawMode, yaw_mode_vk, kPlain}, {kCycleMode, mode_cycle_vk, kPlain},
        {kToggle, 'Y', kCtrlShift},   {kYawMode, 'H', kCtrlShift},     {kCycleMode, 'G', kCtrlShift},
    };
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Hand copied from v0.1.1, unchanged at the frozen reader's commit: src/plugin.cpp:28-29
// (tracking on as EnableOnStartup says, the yaw mode from WorldSpaceYaw), src/tracker_feed.cpp:
// 14-28 (sensitivity, inversion and deadzone onto the rotation processor) and 33-52 (the world
// scale signed by InvertX/Y/Z, rotation and position or rotation only as [Position] Enabled
// says, the smoothing pair), src/position_mapping.h:13-46 (the position sensitivity, LimitY
// on both vertical bounds, the processor's own inversion left off), and src/camera_hook.cpp:167
// and src/flashlight_hook.cpp, which always turned the flashlight with the head at core's
// kDefaultLightMultiplier.
template <class C>
Observed ObservePublished(const C& c) {
    Observed o;
    o.port = c.port;
    o.start_enabled = c.enabled_on_startup;
    o.start_world_yaw = c.world_space_yaw;
    o.start_mode = static_cast<int>(c.pos_enabled ? TrackingMode::RotationAndPosition : TrackingMode::RotationOnly);
    o.sens_yaw = c.sens_yaw;
    o.sens_pitch = c.sens_pitch;
    o.sens_roll = c.sens_roll;
    o.invert_yaw = c.invert_yaw;
    o.invert_pitch = c.invert_pitch;
    o.invert_roll = c.invert_roll;
    o.deadzone_yaw = c.deadzone_yaw;
    o.deadzone_pitch = c.deadzone_pitch;
    o.deadzone_roll = c.deadzone_roll;
    o.local_smoothing = c.local_smoothing;
    o.remote_smoothing = c.remote_smoothing;
    o.pos_sens_x = c.pos_sens_x;
    o.pos_sens_y = c.pos_sens_y;
    o.pos_sens_z = c.pos_sens_z;
    o.limit_x = c.pos_limit_x;
    o.limit_y = c.pos_limit_y;
    o.limit_y_down = c.pos_limit_y;
    o.limit_z = c.pos_limit_z;
    o.limit_z_back = c.pos_limit_z_back;
    o.scale_x = c.pos_world_scale * (c.pos_invert_x ? -1.0f : 1.0f);
    o.scale_y = c.pos_world_scale * (c.pos_invert_y ? -1.0f : 1.0f);
    o.scale_z = c.pos_world_scale * (c.pos_invert_z ? -1.0f : 1.0f);
    o.fov = c.fov_override;
    o.log_to_file = c.log_to_file;
    o.light_follows_head = true;
    o.light_multiplier = cameraunlock::effects::kDefaultLightMultiplier;
    o.hotkeys = LegacyHotkeys(c.toggle_vk, c.yaw_mode_vk, c.mode_cycle_vk);
    return o;
}

Observed ReadOracle(const std::string& dir) { return ObservePublished(bms_published::Start(dir)); }

Observed ReadImport(const std::string& ini) {
    headtracking::legacy::Config c;
    headtracking::legacy::Read(ini.c_str(), c);
    return ObservePublished(c);
}

// ---- Inputs ----------------------------------------------------------------------------------

fs::path DataPath(const char* name) { return SourcePath(std::string("tests/config_differential/data/") + name); }

// The published build's first-run file, extracted once from what its WriteDefaultIni writes and
// committed. FirstRunFileIsThePublishedBuilds holds it to that.
std::string FirstRunFile() { return ReadFileBytes(DataPath("v0.1.1-first-run.ini")); }

// Every key the frozen reader takes a value from, and how the corpus varies each one. The
// out-of-range values sit outside the range each key is refused or clamped outside, and a
// limit above the canonical rows' 10 metres. [Smoothing] Amount and [Position] Smoothing are
// read only to warn that they are ignored, so they are not among them.
std::vector<testing::MutationKey> CorpusKeys() {
    return {
        {"Network", "Port", "5252", {"0", "65536"}},
        {"Network", "EnableOnStartup", "0", {}},
        {"Sensitivity", "Yaw", "1.5", {}},
        {"Sensitivity", "Pitch", "0.5", {}},
        {"Sensitivity", "Roll", "2.0", {}},
        {"Sensitivity", "InvertYaw", "1", {}},
        {"Sensitivity", "InvertPitch", "1", {}},
        {"Sensitivity", "InvertRoll", "1", {}},
        {"Smoothing", "LocalSmoothing", "0.3", {"-0.5", "1.5"}},
        {"Smoothing", "RemoteSmoothing", "0.6", {"-0.5", "1.5"}},
        {"Deadzone", "Yaw", "2.0", {"-0.5"}},
        {"Deadzone", "Pitch", "1.5", {"-0.5"}},
        {"Deadzone", "Roll", "0.5", {"-0.5"}},
        {"Position", "Enabled", "0", {}},
        {"Position", "WorldScale", "50.0", {"-5"}},
        {"Position", "SensX", "2.0", {"-1.5"}},
        {"Position", "SensY", "3.0", {"-1.5"}},
        {"Position", "SensZ", "4.0", {"-1.5"}},
        {"Position", "InvertX", "1", {}},
        {"Position", "InvertY", "1", {}},
        {"Position", "InvertZ", "1", {}},
        {"Position", "LimitX", "0.25", {"-0.1", "11"}},
        {"Position", "LimitY", "0.3", {"-0.1", "11"}},
        {"Position", "LimitZ", "0.5", {"-0.1", "11"}},
        {"Position", "LimitZBack", "0.2", {"-0.1", "11"}},
        {"Hotkeys", "Toggle", "0x70", {"0x59", "0xFF"}, true},
        {"Hotkeys", "YawMode", "0x71", {"0x48", "0xFF"}, true},
        {"Hotkeys", "ModeCycle", "0x72", {"0x47", "0xFF"}, true},
        {"View", "WorldSpaceYaw", "0", {}},
        {"View", "Fov", "90", {"29", "151"}},
        {"Debug", "LogToFile", "0", {}},
    };
}

// The generator refuses the call when these and the descriptors name different keys, so the
// corpus covers every key the import reads.
std::vector<cfg::LegacyKey> CorpusReads() { return headtracking::MakeLegacyImport().keys; }

struct Input {
    std::string name;
    bool present;
    std::string bytes;
};

// The first-run file with [Hotkeys] `key` set to `code`.
std::string WithHotkey(const char* key, int shipped, int code) {
    std::string bytes = FirstRunFile();
    char line[64];
    std::snprintf(line, sizeof line, "%s=0x%X\r\n", key, shipped);
    const std::size_t at = bytes.find(line);
    if (at == std::string::npos) throw std::runtime_error(std::string("the first-run file has no line ") + line);
    char value[64];
    std::snprintf(value, sizeof value, "%s=0x%02X\r\n", key, code);
    return bytes.replace(at, std::strlen(line), value);
}

std::vector<Input> Inputs() {
    std::vector<Input> inputs = {
        {"v0.1.1 first-run file", true, FirstRunFile()},
        {"no file", false, {}},
        {"empty file", true, {}},
    };
    for (testing::IniMutation& m : testing::GenerateIniMutations(FirstRunFile(), CorpusReads(), CorpusKeys())) {
        inputs.push_back({"corpus: " + m.name, true, std::move(m.bytes)});
    }
    const std::pair<const char*, int> hotkeys[] = {{"Toggle", 0x23}, {"YawMode", 0x22}, {"ModeCycle", 0x21}};
    for (const auto& [key, shipped] : hotkeys) {
        for (int code = 0x01; code <= 0xFE; ++code) {
            char name[48];
            std::snprintf(name, sizeof name, "%s=0x%02X", key, code);
            inputs.push_back({name, true, WithHotkey(key, shipped, code)});
        }
    }
    return inputs;
}

// ---- Checks ----------------------------------------------------------------------------------

// The first-run file committed as test data is what the published build writes. On a mismatch
// the published bytes are left in build/ for a look.
void FirstRunFileIsThePublishedBuilds() {
    Scratch s;
    bms_published::Start(s.dir());
    const std::string written = ReadFileBytes(s.ini());
    const bool same = written == FirstRunFile();
    if (!same) {
        std::ofstream(SourcePath("build/v0.1.1-first-run.actual.ini"), std::ios::binary) << written;
    }
    Check(same, "v0.1.1-first-run.ini is what the published build writes at first run");
}

// Comparison 1. Nothing may differ, floats bit for bit.
void OracleAgainstImport(const std::vector<Input>& inputs) {
    int compared = 0;
    for (const Input& input : inputs) {
        Scratch for_oracle;
        Scratch for_import;
        if (input.present) {
            for_oracle.Write(input.bytes);
            for_import.Write(input.bytes);
        }
        const std::vector<std::string> diff =
            Differences(ReadOracle(for_oracle.dir()), ReadImport(for_import.ini()));
        for (const std::string& d : diff) std::printf("  comparison 1, %s: %s\n", input.name.c_str(), d.c_str());
        Check(diff.empty(), "comparison 1: oracle and import agree on " + input.name);
        ++compared;
    }
    std::printf("comparison 1: %d inputs\n", compared);
}

// What the session runs on from a canonical Config: plugin.cpp Initialize and
// tracker_feed.cpp Start (the mode from the pair, the smoothing pair, MakePositionSettings, the
// rotation processor left at identity, kWorldUnitsPerMetre on every axis), camera_hook.cpp and
// flashlight_hook.cpp (the light rows), and hotkey_handler.cpp, which puts each list through
// ParseKeyBindings and RegisterKeyBindings.
Observed ObserveCanonical(const headtracking::Config& c) {
    Observed o;
    o.port = c.udp_port;
    o.start_enabled = c.enable_on_startup;
    o.start_world_yaw = c.world_space_yaw;
    o.start_mode = static_cast<int>(cameraunlock::DecodeTrackingMode(c.rotation_enabled, c.position_enabled).value());
    o.sens_yaw = 1.0f;
    o.sens_pitch = 1.0f;
    o.sens_roll = 1.0f;
    o.local_smoothing = c.local_smoothing;
    o.remote_smoothing = c.remote_smoothing;
    const cameraunlock::PositionSettings ps = headtracking::MakePositionSettings(c);
    o.pos_sens_x = ps.sensitivity_x;
    o.pos_sens_y = ps.sensitivity_y;
    o.pos_sens_z = ps.sensitivity_z;
    o.limit_x = ps.limit_x;
    o.limit_y = ps.limit_y;
    o.limit_y_down = ps.limit_y_down;
    o.limit_z = ps.limit_z;
    o.limit_z_back = ps.limit_z_back;
    o.scale_x = headtracking::kWorldUnitsPerMetre * (ps.invert_x ? -1.0f : 1.0f);
    o.scale_y = headtracking::kWorldUnitsPerMetre * (ps.invert_y ? -1.0f : 1.0f);
    o.scale_z = headtracking::kWorldUnitsPerMetre * (ps.invert_z ? -1.0f : 1.0f);
    o.fov = c.fov_override;
    o.log_to_file = c.log_to_file;
    o.light_follows_head = c.light.follows_head;
    o.light_multiplier = c.light.multiplier;
    const std::pair<Action, const std::string*> lists[] = {
        {kToggle, &c.toggle_key_name}, {kCycleMode, &c.cycle_tracking_mode_key_name}, {kYawMode, &c.yaw_mode_key_name}};
    for (const auto& [action, list] : lists) {
        const cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(*list);
        Check(parsed.ok(), "a migrated key list parses: " + *list);
        for (const cameraunlock::input::KeyBinding& b : parsed.bindings) {
            o.hotkeys.push_back({action, b.vk, static_cast<unsigned>(b.modifiers)});
        }
    }
    std::sort(o.hotkeys.begin(), o.hotkeys.end());
    return o;
}

using Drop = std::tuple<cfg::DropRule, std::string, std::string>;

// The canonical position limits take 0 to 10 metres.
constexpr float kMaxLimit = 10.0f;

// The drops the approved changes call for, from what the frozen reader read, and the settings
// the session then runs on: comparison 2's whole allowance.
struct Allowed {
    std::vector<Drop> dropped;
    Observed observed;
};

Allowed ApplyApprovedChanges(const headtracking::legacy::Config& read) {
    Allowed a;
    a.observed = ObservePublished(read);
    Observed& o = a.observed;
    const auto shaping = [&a](auto value, auto shipped, const char* section, const char* key) {
        if (value != shipped) a.dropped.push_back({cfg::DropRule::PoseShaping, section, key});
    };
    shaping(read.sens_yaw, 1.0f, "Sensitivity", "Yaw");
    shaping(read.sens_pitch, 1.0f, "Sensitivity", "Pitch");
    shaping(read.sens_roll, 1.0f, "Sensitivity", "Roll");
    shaping(read.invert_yaw, false, "Sensitivity", "InvertYaw");
    shaping(read.invert_pitch, false, "Sensitivity", "InvertPitch");
    shaping(read.invert_roll, false, "Sensitivity", "InvertRoll");
    shaping(read.deadzone_yaw, 0.0f, "Deadzone", "Yaw");
    shaping(read.deadzone_pitch, 0.0f, "Deadzone", "Pitch");
    shaping(read.deadzone_roll, 0.0f, "Deadzone", "Roll");
    shaping(read.pos_world_scale, 39.37f, "Position", "WorldScale");
    shaping(read.pos_sens_x, 1.0f, "Position", "SensX");
    shaping(read.pos_sens_y, 1.0f, "Position", "SensY");
    shaping(read.pos_sens_z, 1.0f, "Position", "SensZ");
    shaping(read.pos_invert_x, false, "Position", "InvertX");
    shaping(read.pos_invert_y, false, "Position", "InvertY");
    shaping(read.pos_invert_z, false, "Position", "InvertZ");
    o.sens_yaw = o.sens_pitch = o.sens_roll = 1.0f;
    o.invert_yaw = o.invert_pitch = o.invert_roll = false;
    o.deadzone_yaw = o.deadzone_pitch = o.deadzone_roll = 0.0f;
    o.pos_sens_x = o.pos_sens_y = o.pos_sens_z = 1.0f;
    o.scale_x = o.scale_y = o.scale_z = 39.37f;
    // N3: a Ctrl, Shift or Alt code loses its plain binding, and the chord stays.
    const auto modifier = [](int vk) { return (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5); };
    // N4: a limit above the rows' 10 metres runs as 10.
    const std::tuple<float, float*, const char*> limits[] = {{read.pos_limit_x, &o.limit_x, "LimitX"},
                                                             {read.pos_limit_y, &o.limit_y, "LimitY"},
                                                             {read.pos_limit_z, &o.limit_z, "LimitZ"},
                                                             {read.pos_limit_z_back, &o.limit_z_back, "LimitZBack"}};
    for (const auto& [value, field, key] : limits) {
        if (value <= kMaxLimit) continue;
        a.dropped.push_back({cfg::DropRule::NumberOutOfRange, "Position", key});
        *field = kMaxLimit;
    }
    o.limit_y_down = o.limit_y;
    const std::tuple<int, int, const char*> codes[] = {
        {kToggle, read.toggle_vk, "Toggle"}, {kCycleMode, read.mode_cycle_vk, "ModeCycle"}, {kYawMode, read.yaw_mode_vk, "YawMode"}};
    for (const auto& [action, vk, key] : codes) {
        if (!modifier(vk)) continue;
        a.dropped.push_back({cfg::DropRule::ModifierKey, "Hotkeys", key});
        o.hotkeys.erase(std::find(o.hotkeys.begin(), o.hotkeys.end(), Hotkey{action, vk, kPlain}));
    }
    std::sort(a.dropped.begin(), a.dropped.end());
    return a;
}

// ---- Rows that follow Defaults.ini -----------------------------------------------------------

// Every row of the table that follows Defaults.ini: every concept row, since none is PerGame.
const std::set<Concept>& AllRows() {
    static const std::set<Concept> all = {
        Concept::UdpPort,            Concept::EnableOnStartup,      Concept::WorldSpaceYaw,
        Concept::RotationEnabled,    Concept::PositionEnabled,      Concept::LocalSmoothing,
        Concept::RemoteSmoothing,    Concept::PositionLimitX,       Concept::PositionLimitY,
        Concept::PositionLimitYDown, Concept::PositionLimitZ,       Concept::PositionLimitZBack,
        Concept::ToggleKey,          Concept::CycleTrackingModeKey, Concept::YawModeKey,
        Concept::LightFollowsHead,   Concept::LightMultiplier,
    };
    return all;
}

std::vector<Hotkey> HotkeysOf(const Observed& o, int action) {
    std::vector<Hotkey> keys;
    for (const Hotkey& h : o.hotkeys) {
        if (std::get<0>(h) == action) keys.push_back(h);
    }
    return keys;
}

const std::pair<int, Concept> kHotkeyRows[] = {
    {kToggle, Concept::ToggleKey}, {kCycleMode, Concept::CycleTrackingModeKey}, {kYawMode, Concept::YawModeKey}};

// The rows the player never changed: every value the row gives the session is what v0.1.1 ran on
// with no file. The mode pair is both rows or neither.
std::set<Concept> UntouchedRows(const Observed& read, const Observed& shipped) {
    std::set<Concept> changed;
    const auto row = [&changed](bool differs, Concept id) {
        if (differs) changed.insert(id);
    };
    row(read.port != shipped.port, Concept::UdpPort);
    row(read.start_enabled != shipped.start_enabled, Concept::EnableOnStartup);
    row(read.start_world_yaw != shipped.start_world_yaw, Concept::WorldSpaceYaw);
    row(read.start_mode != shipped.start_mode, Concept::RotationEnabled);
    row(read.start_mode != shipped.start_mode, Concept::PositionEnabled);
    row(Bits(read.local_smoothing) != Bits(shipped.local_smoothing), Concept::LocalSmoothing);
    row(Bits(read.remote_smoothing) != Bits(shipped.remote_smoothing), Concept::RemoteSmoothing);
    row(Bits(read.limit_x) != Bits(shipped.limit_x), Concept::PositionLimitX);
    row(Bits(read.limit_y) != Bits(shipped.limit_y), Concept::PositionLimitY);
    row(Bits(read.limit_y_down) != Bits(shipped.limit_y_down), Concept::PositionLimitYDown);
    row(Bits(read.limit_z) != Bits(shipped.limit_z), Concept::PositionLimitZ);
    row(Bits(read.limit_z_back) != Bits(shipped.limit_z_back), Concept::PositionLimitZBack);
    for (const auto& [action, id] : kHotkeyRows) row(HotkeysOf(read, action) != HotkeysOf(shipped, action), id);
    row(read.light_follows_head != shipped.light_follows_head, Concept::LightFollowsHead);
    row(Bits(read.light_multiplier) != Bits(shipped.light_multiplier), Concept::LightMultiplier);
    std::set<Concept> untouched;
    for (const Concept id : AllRows()) {
        if (!changed.count(id)) untouched.insert(id);
    }
    return untouched;
}

std::string Names(const std::set<Concept>& rows) {
    std::string text;
    for (const Concept id : rows) {
        text += (text.empty() ? "" : ", ") + std::string(cfg::schema::kConcepts[static_cast<std::size_t>(id)].name);
    }
    return text.empty() ? "none" : text;
}

// What the session runs on over a Defaults.ini other than the built-in one: `want`, with each row
// the import left to Defaults.ini as `defaults_ini` gives it.
Observed OverDefaults(Observed want, const std::set<Concept>& follows, const Observed& defaults_ini) {
    const auto take = [&follows](Concept id, auto& field, const auto& value) {
        if (follows.count(id)) field = value;
    };
    take(Concept::UdpPort, want.port, defaults_ini.port);
    take(Concept::EnableOnStartup, want.start_enabled, defaults_ini.start_enabled);
    take(Concept::WorldSpaceYaw, want.start_world_yaw, defaults_ini.start_world_yaw);
    take(Concept::PositionEnabled, want.start_mode, defaults_ini.start_mode);
    take(Concept::LocalSmoothing, want.local_smoothing, defaults_ini.local_smoothing);
    take(Concept::RemoteSmoothing, want.remote_smoothing, defaults_ini.remote_smoothing);
    take(Concept::PositionLimitX, want.limit_x, defaults_ini.limit_x);
    take(Concept::PositionLimitY, want.limit_y, defaults_ini.limit_y);
    take(Concept::PositionLimitYDown, want.limit_y_down, defaults_ini.limit_y_down);
    take(Concept::PositionLimitZ, want.limit_z, defaults_ini.limit_z);
    take(Concept::PositionLimitZBack, want.limit_z_back, defaults_ini.limit_z_back);
    take(Concept::LightFollowsHead, want.light_follows_head, defaults_ini.light_follows_head);
    take(Concept::LightMultiplier, want.light_multiplier, defaults_ini.light_multiplier);
    std::vector<Hotkey> keys;
    for (const auto& [action, id] : kHotkeyRows) {
        const std::vector<Hotkey> from = HotkeysOf(follows.count(id) ? defaults_ini : want, action);
        keys.insert(keys.end(), from.begin(), from.end());
    }
    std::sort(keys.begin(), keys.end());
    want.hotkeys = keys;
    return want;
}

std::vector<std::string> CanonicalDiagnostics(const std::string& bytes, headtracking::Config& out) {
    std::vector<std::string> found;
    const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(bytes);
    for (const cfg::CanonicalDiagnostic& d : doc.diagnostics) found.push_back("reader: " + cfg::DescribeCanonicalDiagnostic(d));
    const cfg::ConfigTable<headtracking::Config> table = headtracking::MakeConfigTable();
    out = table.defaults();
    for (const cfg::CanonicalDiagnostic& d : cfg::ApplyCanonical(doc, table, out).diagnostics) {
        found.push_back("table: " + cfg::DescribeCanonicalDiagnostic(d));
    }
    return found;
}

bool AsciiCrlf(const std::string& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c > 0x7E) return false;
        if (c == '\r' && (i + 1 == bytes.size() || bytes[i + 1] != '\n')) return false;
        if (c == '\n' && (i == 0 || bytes[i - 1] != '\r')) return false;
        if (c < 0x20 && c != '\r' && c != '\n') return false;
    }
    return !bytes.empty() && bytes.back() == '\n';
}

// A file's bytes, last write time and attributes, which no load may change.
struct FileState {
    std::string bytes;
    unsigned long long written = 0;
    DWORD attributes = 0;
    bool operator==(const FileState& other) const {
        return bytes == other.bytes && written == other.written && attributes == other.attributes;
    }
};

std::optional<FileState> StateOf(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return std::nullopt;
        throw std::runtime_error("cannot read the attributes of " + path.string());
    }
    FileState state;
    state.bytes = ReadFileBytes(path);
    state.written = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    state.attributes = data.dwFileAttributes;
    return state;
}

bool LogSays(const std::vector<std::string>& log, const std::string& text) {
    for (const std::string& line : log) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

// A Defaults.ini holding a value other than the built-in one on every global row the table
// binds, so a migration that wrote `default` where the imported value is not what `default`
// gives would read back differently over it.
const char* const kSkewedDefaults =
    "[CameraUnlock]\r\nConfigFormat=1\r\n\r\n"
    "[Network]\r\nUdpPort=5252\r\n\r\n"
    "[General]\r\nEnableOnStartup=false\r\nWorldSpaceYaw=false\r\nRotationEnabled=false\r\n\r\n"
    "[Smoothing]\r\nLocalSmoothing=0.5\r\nRemoteSmoothing=0.5\r\n\r\n"
    "[Position]\r\nPositionEnabled=true\r\nPositionLimitX=0.5\r\nPositionLimitY=0.5\r\nPositionLimitYDown=0.5\r\n"
    "PositionLimitZ=0.5\r\nPositionLimitZBack=0.5\r\n\r\n"
    "[Hotkeys]\r\nToggleKey=F8\r\nCycleTrackingModeKey=F9\r\nYawModeKey=F10\r\n\r\n"
    "[Light]\r\nLightFollowsHead=false\r\nLightMultiplier=0.5\r\n";

// The folder beside this executable the migrated files are written to, for lint-migrated.mjs,
// which CTest runs after this test.
fs::path MigratedFolder() {
    std::vector<wchar_t> exe(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) throw std::runtime_error("cannot find this executable's path");
        if (length < exe.size()) return fs::path(std::wstring(exe.data(), length)).parent_path() / "migrated";
        exe.resize(exe.size() * 2);
    }
}

// Runs the owner's Load in `s`, whose game folder holds the input as HeadTracking.ini or
// nothing, checks what a load must do beyond comparison 2, and returns the settings the session
// runs on. A file it creates by migrating goes into `migrated_files`.
headtracking::Config Migrate(const Input& input, const Scratch& s, const std::string& label,
                             std::set<std::string>& migrated_files) {
    const fs::path legacy = fs::path(s.wini());
    const std::optional<FileState> legacy_before = StateOf(legacy);
    const std::set<std::string> both{"CameraUnlock.ini", "HeadTracking.ini"};

    const cfg::ConfigLoadResult<headtracking::Config> loaded =
        cfg::ConfigOwner<headtracking::Config>(s.Options()).Load();
    Check(StateOf(legacy) == legacy_before, label + ": a load leaves HeadTracking.ini's bytes, write time and attributes");

    const cfg::ConfigLoadStatus want = input.present ? cfg::ConfigLoadStatus::Migrated : cfg::ConfigLoadStatus::Created;
    if (loaded.status != want) {
        std::printf("  %s: %s, %s\n", label.c_str(), cfg::ConfigLoadStatusName(loaded.status), loaded.reason.c_str());
    }
    Check(loaded.status == want, label + ": the legacy file imports, or with none the file is created");
    if (loaded.status != want) return loaded.config;
    Check(s.Names() == (input.present ? both : std::set<std::string>{"CameraUnlock.ini"}),
          label + ": the game folder holds HeadTracking.ini and CameraUnlock.ini and nothing else");
    Check(!input.present || LogSays(loaded.log, "created from"), label + ": the log says where CameraUnlock.ini came from");

    const std::string migrated = ReadFileBytes(s.canonical());
    Check(cfg::HasCanonicalStamp(migrated), label + ": CameraUnlock.ini carries the stamp");
    Check(AsciiCrlf(migrated), label + ": CameraUnlock.ini is ASCII with CRLF line ends");
    if (input.present) migrated_files.insert(migrated);

    // The next start reads CameraUnlock.ini, imports nothing and writes nothing.
    const std::optional<FileState> created = StateOf(s.canonical());
    const cfg::ConfigLoadResult<headtracking::Config> again =
        cfg::ConfigOwner<headtracking::Config>(s.Options()).Load();
    Check(again.status == cfg::ConfigLoadStatus::Canonical, label + ": the next start reads CameraUnlock.ini");
    Check(again.diagnostics.empty(), label + ": CameraUnlock.ini reads with no diagnostic");
    Check(Differences(ObserveCanonical(again.config), ObserveCanonical(loaded.config)).empty(),
          label + ": the next start runs on the same settings");
    Check(StateOf(s.canonical()) == created && StateOf(legacy) == legacy_before,
          label + ": the next start changes neither file");
    Check(!input.present || LogSays(again.log, "is left as it was and is not read"),
          label + ": the next start logs that HeadTracking.ini is not read");
    return loaded.config;
}

// Comparison 2, and what the migration must do with every input besides.
void ImportAgainstMigration(const std::vector<Input>& inputs) {
    const std::string committed = ReadFileBytes(SourcePath("config/CameraUnlock.ini"));
    const cfg::ConfigTable<headtracking::Config> table = headtracking::MakeConfigTable();
    const Observed shipped = ObservePublished(headtracking::legacy::Config{});
    headtracking::Config skewed_config;
    const std::vector<std::string> skewed_diagnostics = CanonicalDiagnostics(kSkewedDefaults, skewed_config);
    for (const std::string& d : skewed_diagnostics) std::printf("  the skewed Defaults.ini: %s\n", d.c_str());
    Check(skewed_diagnostics.empty(), "the skewed Defaults.ini sets every row with no diagnostic");
    const Observed skewed_observed = ObserveCanonical(skewed_config);
    Check(UntouchedRows(skewed_observed, ObserveCanonical(table.defaults())).empty(),
          "the skewed Defaults.ini differs from the built-in values on every row");
    std::set<std::string> migrated_files;
    int compared = 0;
    int clamped_inputs = 0;
    int touched = 0;
    int mode_touched = 0;
    for (const Input& input : inputs) {
        const std::string& name = input.name;

        // The import, run as the owner runs it but on a read-only copy: it reads what the
        // frozen reader reads, drops what the approved changes drop, and writes nothing.
        cfg::ImportResult imported;
        headtracking::legacy::Config read;
        {
            Scratch ro;
            if (input.present) {
                ro.Write(input.bytes);
                SetFileAttributesA(ro.ini().c_str(), FILE_ATTRIBUTE_READONLY);
            }
            const auto before = ro.Listing();
            headtracking::Config unused = table.defaults();
            imported = headtracking::MakeLegacyImport().run({ro.wini(), ro.ini(), false}, unused);
            Check(ro.Listing() == before, name + ": the import leaves a read-only folder as it was");
            headtracking::legacy::Read(ro.ini().c_str(), read);
        }
        Check(imported.status == (input.present ? cfg::ImportStatus::Imported : cfg::ImportStatus::Absent),
              name + ": the import reads every input, as the published build did");

        const Allowed allowed = ApplyApprovedChanges(read);
        std::vector<Drop> dropped;
        for (const cfg::DroppedValue& d : imported.dropped) dropped.push_back({d.rule, d.section, d.key});
        std::sort(dropped.begin(), dropped.end());
        Check(dropped == allowed.dropped, name + ": the import drops exactly what the approved changes drop");
        Check(imported.pose_shaping.size() == 16, name + ": the import records all sixteen pose-shaping settings");
        for (const cfg::PoseShapingValue& p : imported.pose_shaping) {
            const bool changed = std::find(allowed.dropped.begin(), allowed.dropped.end(),
                                           Drop{cfg::DropRule::PoseShaping, p.section, p.key}) != allowed.dropped.end();
            Check(p.folded != changed, name + ": [" + p.section + "] " + p.key + " folds exactly when it is the shipped value");
        }
        const Observed& want = allowed.observed;

        const std::set<Concept> follows(imported.follows_defaults_ini.begin(), imported.follows_defaults_ini.end());
        Check(follows.size() == imported.follows_defaults_ini.size(), name + ": follows_defaults_ini names each row once");
        const std::set<Concept> untouched = UntouchedRows(ObservePublished(read), shipped);
        if (follows != untouched) {
            std::printf("  %s: follows Defaults.ini %s, untouched %s\n", name.c_str(), Names(follows).c_str(),
                        Names(untouched).c_str());
        }
        Check(follows == untouched, name + ": the rows left to Defaults.ini are exactly the ones the player never changed");
        if (untouched != AllRows()) ++touched;
        if (!untouched.count(Concept::RotationEnabled)) ++mode_touched;
        const bool unedited = name == "v0.1.1 first-run file" || name == "empty file" || name == "no file";
        if (unedited) Check(untouched == AllRows(), name + ": every row follows Defaults.ini");

        if (std::any_of(allowed.dropped.begin(), allowed.dropped.end(),
                        [](const Drop& d) { return std::get<0>(d) == cfg::DropRule::NumberOutOfRange; })) {
            ++clamped_inputs;
            Check(input.present, name + ": a limit is clamped with no file");
        }

        const auto compare = [&](const headtracking::Config& got, const std::string& label) {
            const std::vector<std::string> diff = Differences(want, ObserveCanonical(got));
            for (const std::string& d : diff) std::printf("  comparison 2, %s: %s\n", label.c_str(), d.c_str());
            Check(diff.empty(), "comparison 2: the session runs as the import read, less the approved changes: " + label);
        };

        // Over a Defaults.ini the owner creates with the built-in values.
        {
            Scratch s;
            if (input.present) s.Write(input.bytes);
            const headtracking::Config migrated = Migrate(input, s, name, migrated_files);
            compare(migrated, name);
            headtracking::Config reread;
            CanonicalDiagnostics(ReadFileBytes(s.canonical()), reread);
            Check(Differences(ObserveCanonical(reread), ObserveCanonical(migrated)).empty(),
                  name + ": CameraUnlock.ini reads back as the settings the session runs on");

            // Fresh equals upgrade: the published build's first-run file, an empty file and no
            // file at all end as the committed file, `default` on every row.
            if (unedited) {
                Check(ReadFileBytes(s.canonical()) == committed, name + ": gives the committed file byte for byte");
            }
        }

        // From a read-only HeadTracking.ini, which keeps its attribute.
        if (input.present) {
            Scratch ro;
            ro.Write(input.bytes);
            SetFileAttributesA(ro.ini().c_str(), FILE_ATTRIBUTE_READONLY);
            compare(Migrate(input, ro, name + " (read-only)", migrated_files), name + " (read-only)");
            Check((GetFileAttributesA(ro.ini().c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
                  name + ": HeadTracking.ini keeps its read-only attribute");
        }

        // Over a Defaults.ini that differs everywhere: a row the player never changed is
        // `default` and takes Defaults.ini's value, and a changed row keeps the player's. With no
        // legacy file every row is Defaults.ini's, which config_tests covers.
        if (input.present) {
            Scratch skewed;
            skewed.Write(input.bytes);
            skewed.WriteDefaults(kSkewedDefaults);
            const std::string label = name + " (skewed Defaults.ini)";
            const headtracking::Config got = Migrate(input, skewed, label, migrated_files);
            const std::vector<std::string> diff =
                Differences(OverDefaults(want, follows, skewed_observed), ObserveCanonical(got));
            for (const std::string& d : diff) std::printf("  comparison 2, %s: %s\n", label.c_str(), d.c_str());
            Check(diff.empty(), label + ": the untouched rows take Defaults.ini's values and the changed rows keep the import's");
            const std::string migrated = ReadFileBytes(skewed.canonical());
            for (const Concept id : follows) {
                const std::string key = cfg::schema::kConcepts[static_cast<std::size_t>(id)].key;
                Check(migrated.find("\r\n" + key + "=default\r\n") != std::string::npos,
                      label + ": " + key + " is written default");
            }
        }
        ++compared;
    }
    std::printf("comparison 2: %d inputs, %d with a limit above 10 clamped (N4)\n", compared, clamped_inputs);
    std::printf("%d inputs changed a row from v0.1.1's default, %d of them the tracking mode\n", touched, mode_touched);
    Check(clamped_inputs > 0, "the corpus reaches a limit above the canonical rows' 10");
    Check(touched > 0 && mode_touched > 0,
          "the inputs change rows, the tracking mode among them, which then do not follow Defaults.ini");

    // Core's canonical config lint runs over these next (lint-migrated.mjs).
    const fs::path lint = MigratedFolder();
    fs::remove_all(lint);
    fs::create_directories(lint);
    std::size_t n = 0;
    for (const std::string& file : migrated_files) {
        std::ofstream out(lint / (std::to_string(n++) + ".ini"), std::ios::binary | std::ios::trunc);
        out.write(file.data(), static_cast<std::streamsize>(file.size()));
        if (!out) throw std::runtime_error("cannot write a migrated file under " + lint.string());
    }
    std::printf("%zu distinct migrated files written to %s\n", migrated_files.size(), lint.string().c_str());
}

}  // namespace

int main() {
    // Unbuffered, so the lines before an uncaught exception reach the log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SourcesAreThePinnedOnes();
    FirstRunFileIsThePublishedBuilds();
    const std::vector<Input> inputs = Inputs();
    OracleAgainstImport(inputs);
    ImportAgainstMigration(inputs);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
