#include "RootTool.h"
#include "resources.h"
#include <imgui.h>
#include <windows.h>
#include <TlHelp32.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cmath>

#include "VHDManager.h"
#include <thread>

namespace fs = std::filesystem;


static constexpr float kTitleH     = 52.0f;
static constexpr float kStatusBarH = 32.0f;
static constexpr float kBtnH       = 46.0f;
static constexpr float kCornerR    = 10.0f;
static constexpr float kInnerR     = 6.0f;
static constexpr float kPI         = 3.14159265f;
static constexpr float kAnimSpeed  = 6.0f;
static constexpr float kHoverSpeed = 10.0f;

static constexpr ImVec4 kAccent = { 0.0f,  0.784f, 0.588f, 1.0f };
static constexpr ImVec4 kError  = { 1.0f,  0.38f,  0.22f,  1.0f };


static float Lerp(float a, float b, float t)    { return a + (b - a) * t; }
static float Clamp01(float v)                   { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static void KillProcessByName(const char* name)
{
    wchar_t wname[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, MAX_PATH);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, wname) == 0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    }
    CloseHandle(snap);
}


RootTool::RootTool()
{
    RefreshEmulatorInfo();
    m_lastFrameTime = std::chrono::steady_clock::now();
}


void RootTool::Log(const std::string& /*msg*/, bool /*isError*/) {}

void RootTool::SetStatus(const std::string& msg, bool isError)
{
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    m_pendingMsg     = msg;
    m_pendingIsError = isError;
    m_hasPending.store(true);
}


std::string RootTool::ReadRegistryString(const std::string& subKey, const std::string& valueName)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};

    char  val[MAX_PATH]{};
    DWORD len  = sizeof(val);
    DWORD type;
    bool  ok   = RegQueryValueExA(hKey, valueName.c_str(), nullptr, &type, (LPBYTE)val, &len) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return ok ? std::string(val) : std::string{};
}


void RootTool::FindInstances(EmulatorInfo& info)
{
    info.instances.clear();
    if (info.dataDir.empty()) return;

    std::string engineDir = info.dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/')
        engineDir += '\\';

    std::string lower = engineDir;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    if (lower.find("engine\\") == std::string::npos && lower.find("engine/") == std::string::npos)
        engineDir += "Engine\\";

    if (!fs::exists(engineDir)) return;

    std::string metaPath = engineDir + "UserData\\MimMetaData.json";
    if (fs::exists(metaPath)) {
        std::string content = ReadFileString(metaPath);
        std::regex  blockRx(R"(\{([^{}]*\"InstanceName\"[^{}]*)\})");
        std::regex  nameRx(R"(\"Name\"\s*:\s*\"([^\"]+)\")");
        std::regex  instRx(R"(\"InstanceName\"\s*:\s*\"([^\"]+)\")");

        auto it  = std::sregex_iterator(content.begin(), content.end(), blockRx);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            std::string block = it->str(1);
            std::smatch nm, im;
            if (!std::regex_search(block, nm, nameRx) || !std::regex_search(block, im, instRx))
                continue;

            BstkInstance inst{ nm[1].str(), im[1].str() };
            bool dup = false;
            for (const auto& e : info.instances) if (e.instanceName == inst.instanceName) { dup = true; break; }
            if (!dup) info.instances.push_back(inst);
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(engineDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name == "Manager" || name == "UserData") continue;

        bool hasBstk = false;
        for (const auto& f : fs::directory_iterator(entry.path(), ec))
            if (f.path().extension() == ".bstk") { hasBstk = true; break; }

        if (!hasBstk) continue;

        bool dup = false;
        for (const auto& inst : info.instances) if (inst.instanceName == name) { dup = true; break; }
        if (!dup) info.instances.push_back({ name, name });
    }
}

void RootTool::RefreshEmulatorInfo()
{
    auto detect = [&](EmulatorInfo& e, bool isBstk, const char* regKey, const char* label) {
        e.isBlueStacks = isBstk;
        e.name         = label;
        e.installDir   = ReadRegistryString(regKey, "InstallDir");
        e.dataDir      = ReadRegistryString(regKey, "DataDir");
        FindInstances(e);
    };

    detect(m_bluestacks, true,  "SOFTWARE\\BlueStacks_nxt",  "BlueStacks 5");
    detect(m_msi,        false, "SOFTWARE\\BlueStacks_msi5", "MSI App Player");

    if (!m_bluestacks.instances.empty())     m_selectedInstance = m_bluestacks.instances[0].instanceName;
    else if (!m_msi.instances.empty())       m_selectedInstance = m_msi.instances[0].instanceName;
}


std::string RootTool::ReadFileString(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

bool RootTool::WriteFileString(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    return f.good();
}

bool RootTool::IsMasterInstance(const std::string& name)
{
    return !std::regex_match(name, std::regex(".*_\\d+$"));
}

std::string RootTool::GetMasterInstanceName(const std::string& name)
{
    std::smatch m;
    if (std::regex_match(name, m, std::regex("^(.+)_\\d+$")))
        return m[1].str();
    return name;
}


void RootTool::KillProcesses()
{
    KillProcessByName("HD-Player.exe");
    KillProcessByName("HD-MultiInstanceManager.exe");
    KillProcessByName("BstkSVC.exe");
    SetStatus("Emulator processes stopped.", false);
}

void RootTool::PatchHDPlayer(const std::string& installDir)
{
    if (installDir.empty()) return;

    KillProcesses();
    ::Sleep(1000);

    std::string exePath = installDir + "HD-Player.exe";
    std::string bakPath = exePath    + ".bak";

    if (!fs::exists(exePath)) {
        SetStatus("HD-Player.exe not found.", true);
        return;
    }

    if (!fs::exists(bakPath)) {
        std::error_code ec;
        fs::copy_file(exePath, bakPath, ec);
        if (ec) { SetStatus("Backup failed: " + ec.message(), true); return; }
    }

    std::ifstream f(exePath, std::ios::binary | std::ios::ate);
    if (!f) { SetStatus("Cannot open HD-Player.exe — run as Administrator.", true); return; }
    std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
    f.close();

    auto rd32 = [&](size_t off) -> uint32_t {
        if (off + 4 > buf.size()) return 0;
        return (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) | ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
    };
    auto rd16 = [&](size_t off) -> uint16_t {
        if (off + 2 > buf.size()) return 0;
        return (uint16_t)buf[off] | ((uint16_t)buf[off+1]<<8);
    };

    if (buf.size() < 0x40) { SetStatus("File too small — not a valid PE.", true); return; }
    size_t peOff = rd32(0x3C);
    if (peOff + 24 >= buf.size() || buf[peOff] != 'P' || buf[peOff+1] != 'E') {
        SetStatus("Invalid PE header.", true); return;
    }

    int    numSections = rd16(peOff + 6);
    size_t optHdrSize  = rd16(peOff + 20);
    size_t secTableOff = peOff + 24 + optHdrSize;

    struct Section { char name[9]{}; uint32_t va, vsz, rawOff, rawSz; };
    std::vector<Section> sections;
    for (int i = 0; i < numSections; i++) {
        size_t sh = secTableOff + (size_t)i * 40;
        if (sh + 40 > buf.size()) break;
        Section s;
        memcpy(s.name, &buf[sh], 8);
        s.vsz    = rd32(sh + 8);
        s.va     = rd32(sh + 12);
        s.rawSz  = rd32(sh + 16);
        s.rawOff = rd32(sh + 20);
        sections.push_back(s);
    }

    const Section* textSec = nullptr;
    for (auto& s : sections) if (strncmp(s.name, ".text", 5) == 0) { textSec = &s; break; }
    if (!textSec) { SetStatus(".text section not found.", true); return; }

    size_t textStart = textSec->rawOff;
    size_t textEnd   = textSec->rawOff + textSec->rawSz;

    auto rvaToFile = [&](uint32_t rva) -> size_t {
        for (auto& s : sections)
            if (rva >= s.va && rva < s.va + s.vsz)
                return s.rawOff + (rva - s.va);
        return (size_t)-1;
    };

    auto findString = [&](const char* str, size_t start = 0, size_t end = (size_t)-1) {
        std::vector<size_t> hits;
        size_t len = strlen(str);
        if (end > buf.size()) end = buf.size();
        if (!len || start + len > end) return hits;
        for (size_t i = start; i + len <= end; i++)
            if (memcmp(&buf[i], str, len) == 0) hits.push_back(i);
        return hits;
    };

    auto isTestJzAfterCall = [&](size_t off) -> bool {
        if (off + 4 > buf.size()) return false;
        if (buf[off] != 0x84 || buf[off+1] != 0xC0 || buf[off+2] != 0x74) return false;
        return off >= 5 && buf[off - 5] == 0xE8;
    };

    size_t      patchOffset = std::string::npos;
    std::string patchMethod;

    auto tryAnchor = [&](const char* anchor, const char* desc, int backRange) -> bool {
        for (size_t strOff : findString(anchor)) {
            uint32_t strRva = 0;
            for (auto& s : sections)
                if (strOff >= s.rawOff && strOff < s.rawOff + s.rawSz)
                    { strRva = s.va + (uint32_t)(strOff - s.rawOff); break; }
            if (!strRva) continue;

            for (size_t i = textStart; i + 7 <= textEnd; i++) {
                bool isLea = false; int leaLen = 7;
                if ((buf[i] == 0x48 || buf[i] == 0x4C) && buf[i+1] == 0x8D && (buf[i+2] & 0xC7) == 0x05)
                    isLea = true;
                else if (buf[i] == 0x8D && (buf[i+1] & 0xC7) == 0x05)
                    { isLea = true; leaLen = 6; }
                if (!isLea) continue;

                uint32_t targetRva = (textSec->va + (uint32_t)(i - textStart)) + leaLen + (int32_t)rd32(i + leaLen - 4);
                if (targetRva != strRva) continue;

                size_t searchFrom = (i > textStart + (size_t)backRange) ? i - backRange : textStart;
                for (size_t j = i - 2; j >= searchFrom && j < i; j--) {
                    if (isTestJzAfterCall(j)) { patchOffset = j; patchMethod = desc; return true; }
                }
            }
        }
        return false;
    };

    if (!tryAnchor("Verified the disk integrity!",                            "anchor: verify string",   80))
    if (!tryAnchor("plrDiskCheckThreadEntry",                                 "anchor: thread entry",    0x700))
    if (!tryAnchor("Shutting down: disk file have been illegally tampered with!", "anchor: tamper msg", 0x700))
    {
        std::vector<size_t> candidates;
        for (size_t i = textStart + 5; i + 4 <= textEnd; i++)
            if (isTestJzAfterCall(i)) candidates.push_back(i);

        if (candidates.size() == 1) {
            patchOffset = candidates[0];
            patchMethod = "full .text scan (unique)";
        } else if (candidates.size() > 1) {
            auto verifyHits = findString("Verified the disk integrity!");
            auto failHits   = findString("Failed to verify the disk integrity!");
            for (size_t cand : candidates) {
                for (size_t k = cand; k < cand + 200 && k + 7 <= textEnd; k++) {
                    if ((buf[k] == 0x48 || buf[k] == 0x4C) && buf[k+1] == 0x8D && (buf[k+2] & 0xC7) == 0x05) {
                        uint32_t rva = (textSec->va + (uint32_t)(k - textStart)) + 7 + (int32_t)rd32(k + 3);
                        size_t   off = rvaToFile(rva);
                        for (auto h : verifyHits) if (off == h) { patchOffset = cand; patchMethod = "scan+validate"; goto done; }
                        for (auto h : failHits)   if (off == h) { patchOffset = cand; patchMethod = "scan+validate"; goto done; }
                    }
                }
            }
        }
    }
done:

    if (patchOffset == std::string::npos) {
        SetStatus("Patch site not found. Unsupported version?", true);
        return;
    }

    if (buf[patchOffset+2] == 0x90 && buf[patchOffset+3] == 0x90) {
        SetStatus("Already patched — nothing to do.", false);
        return;
    }

    buf[patchOffset + 2] = 0x90;
    buf[patchOffset + 3] = 0x90;

    std::ofstream out(exePath, std::ios::binary);
    if (!out || !out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size()))
        SetStatus("Write failed — run as Administrator.", true);
    else
        SetStatus("Patched successfully! (" + patchMethod + ")", false);
}

void RootTool::ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName)
{
    if (dataDir.empty() || instanceName.empty()) return;

    KillProcesses();
    ::Sleep(1000);

    std::string engineDir   = dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string bstkPath    = engineDir + instanceName + "\\" + instanceName + ".bstk";

    if (!fs::exists(bstkPath)) { SetStatus(".bstk file not found.", true); return; }

    std::string content = ReadFileString(bstkPath);
    std::string result;
    result.reserve(content.size());

    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        bool target = (line.find("location=\"fastboot.vdi\"") != std::string::npos ||
                       line.find("location=\"Root.vhd\"")     != std::string::npos) &&
                       line.find("type=\"Readonly\"")         != std::string::npos;
        if (target) line = std::regex_replace(line, std::regex("type=\"Readonly\""), "type=\"Normal\"");
        result += line + "\n";
    }

    if (!WriteFileString(bstkPath, result))
        SetStatus("Failed to update .bstk — run as Administrator.", true);
    else
        SetStatus("Disk set to R/W.", false);
}

void RootTool::RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName)
{
    if (dataDir.empty() || instanceName.empty()) return;

    KillProcesses();
    ::Sleep(1000);

    std::string engineDir   = dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string bstkPath    = engineDir + instanceName + "\\" + instanceName + ".bstk";

    if (!fs::exists(bstkPath)) { SetStatus(".bstk file not found.", true); return; }

    std::string content = ReadFileString(bstkPath);
    std::string result;
    result.reserve(content.size());

    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        bool target = (line.find("location=\"fastboot.vdi\"") != std::string::npos ||
                       line.find("location=\"Root.vhd\"")     != std::string::npos) &&
                       line.find("type=\"Normal\"")           != std::string::npos;
        if (target) line = std::regex_replace(line, std::regex("type=\"Normal\""), "type=\"Readonly\"");
        result += line + "\n";
    }

    if (!WriteFileString(bstkPath, result))
        SetStatus("Failed to update .bstk — run as Administrator.", true);
    else
        SetStatus("Disk reverted to Readonly.", false);
}

void RootTool::OneClickRoot(const std::string& dataDir, const std::string& selectedInstance)
{
    if (dataDir.empty() || selectedInstance.empty()) return;

    std::string masterInst  = GetMasterInstanceName(selectedInstance);

    KillProcesses();
    ::Sleep(1000);

    std::string engineDir   = dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string vhdPath     = engineDir + masterInst + "\\Root.vhd";

    if (!fs::exists(vhdPath)) { SetStatus("Root.vhd not found.", true); return; }

    HRSRC   hRes  = FindResource(nullptr, MAKEINTRESOURCE(IDR_SU_BINARY), RT_RCDATA);
    HGLOBAL hData = hRes ? LoadResource(nullptr, hRes) : nullptr;
    DWORD   suSz  = hRes ? SizeofResource(nullptr, hRes) : 0;
    const uint8_t* suRaw = hData ? (const uint8_t*)LockResource(hData) : nullptr;
    if (!suRaw || !suSz) { SetStatus("Internal error: su resource missing.", true); return; }

    std::vector<uint8_t> suBin(suSz);
    for (DWORD i = 0; i < suSz; i++) suBin[i] = suRaw[i] ^ 0xA7;

    char tempDir[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tempDir);
    std::string tmpSu = std::string(tempDir) + "bstk_su.tmp";
    {
        std::ofstream tmp(tmpSu, std::ios::binary);
        if (!tmp) { SetStatus("Failed to create temp file.", true); return; }
        tmp.write(reinterpret_cast<const char*>(suBin.data()), suSz);
    }

    VHDManager vhd;
    if (!vhd.OpenVHD(vhdPath)) { SetStatus("Failed to open VHD: " + vhd.GetLastError(), true); return; }

    int ext4Idx = -1;
    for (size_t i = 0; i < vhd.GetPartitions().size(); i++)
        if (vhd.GetPartitions()[i].is_ext4 && ext4Idx < 0) ext4Idx = (int)i;

    if (ext4Idx < 0) {
        SetStatus("No ext4 partition found in VHD.", true);
        vhd.CloseVHD(); return;
    }

    if (!vhd.MountExt4Partition(ext4Idx)) {
        SetStatus("Failed to mount ext4: " + vhd.GetLastError(), true);
        vhd.CloseVHD(); return;
    }

    vhd.MakeDirectory("/android/system/xbin");

    const std::string suDest = "/android/system/xbin/su";
    if (!vhd.CopyFileFromHost(tmpSu, suDest)) {
        SetStatus("Failed to write su binary.", true);
        vhd.UnmountExt4(); vhd.CloseVHD(); return;
    }

    vhd.SetFilePermissions(suDest, 06755);
    vhd.SetFileOwner(suDest, 0, 0);

    vhd.UnmountExt4();
    vhd.CloseVHD();
    fs::remove(tmpSu);

    std::string confDir = dataDir;
    if (confDir.back() != '\\' && confDir.back() != '/') confDir += '\\';
    {
        std::string lower = confDir;
        for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
        size_t ePos = lower.find("engine\\");
        if (ePos != std::string::npos && ePos + 7 == lower.size())
            confDir = confDir.substr(0, ePos);
    }
    std::string confPath = confDir + "bluestacks.conf";
    if (fs::exists(confPath)) {
        std::string key  = "bst.instance." + selectedInstance + ".enable_root_access=";
        std::string conf = ReadFileString(confPath);
        std::string out;
        bool replaced    = false;
        std::istringstream ss(conf);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find(key) != std::string::npos) { line = key + "\"0\""; replaced = true; }
            out += line + "\n";
        }
        if (!replaced) out += key + "\"0\"\n";
        WriteFileString(confPath, out);
    }

    SetStatus("Rooted successfully!", false);
}

void RootTool::OneClickUnroot(const std::string& dataDir, const std::string& selectedInstance)
{
    if (dataDir.empty() || selectedInstance.empty()) return;

    std::string masterInst = GetMasterInstanceName(selectedInstance);

    KillProcesses();
    ::Sleep(1000);

    std::string engineDir  = dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string vhdPath    = engineDir + masterInst + "\\Root.vhd";

    if (!fs::exists(vhdPath)) { SetStatus("Root.vhd not found.", true); return; }

    VHDManager vhd;
    if (!vhd.OpenVHD(vhdPath)) { SetStatus("Failed to open VHD: " + vhd.GetLastError(), true); return; }

    int ext4Idx = -1;
    for (size_t i = 0; i < vhd.GetPartitions().size(); i++)
        if (vhd.GetPartitions()[i].is_ext4 && ext4Idx < 0) ext4Idx = (int)i;

    if (ext4Idx < 0) { SetStatus("No ext4 partition found.", true); vhd.CloseVHD(); return; }

    if (!vhd.MountExt4Partition(ext4Idx)) {
        SetStatus("Failed to mount ext4: " + vhd.GetLastError(), true);
        vhd.CloseVHD(); return;
    }

    const std::string suPath = "/android/system/xbin/su";
    if (vhd.FileExists(suPath) && !vhd.DeleteFile(suPath))
        SetStatus("Failed to remove su binary.", true);

    vhd.UnmountExt4();
    vhd.CloseVHD();
    SetStatus("Unrooted successfully!", false);
}


void RootTool::RunAsync(std::function<void()> fn)
{
    if (m_busy.load()) return;
    m_busy.store(true);
    std::thread([this, fn = std::move(fn)] {
        fn();
        m_busy.store(false);
    }).detach();
}


void RootTool::SetupTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = kCornerR;
    s.WindowPadding     = { 0, 0 };
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 5.0f;
    s.ScrollbarRounding = 5.0f;
    s.FramePadding      = { 10, 7 };
    s.ItemSpacing       = { 10, 8 };
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupRounding     = 6.0f;
    s.PopupBorderSize   = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = { 0,     0,     0,     0     };
    c[ImGuiCol_ChildBg]              = { 0,     0,     0,     0     };
    c[ImGuiCol_Border]               = { 0.11f, 0.14f, 0.19f, 1.0f  };
    c[ImGuiCol_FrameBg]              = { 0.06f, 0.08f, 0.11f, 1.0f  };
    c[ImGuiCol_FrameBgHovered]       = { 0.09f, 0.12f, 0.17f, 1.0f  };
    c[ImGuiCol_FrameBgActive]        = { 0.12f, 0.16f, 0.23f, 1.0f  };
    c[ImGuiCol_Button]               = { 0.06f, 0.08f, 0.11f, 1.0f  };
    c[ImGuiCol_ButtonHovered]        = { 0.09f, 0.12f, 0.17f, 1.0f  };
    c[ImGuiCol_ButtonActive]         = { 0.0f,  0.784f,0.588f,1.0f  };
    c[ImGuiCol_Header]               = { 0.06f, 0.08f, 0.11f, 1.0f  };
    c[ImGuiCol_HeaderHovered]        = { 0.09f, 0.12f, 0.17f, 1.0f  };
    c[ImGuiCol_HeaderActive]         = { 0.12f, 0.16f, 0.23f, 1.0f  };
    c[ImGuiCol_CheckMark]            = { 0.0f,  0.784f,0.588f,1.0f  };
    c[ImGuiCol_SliderGrab]           = { 0.0f,  0.784f,0.588f,1.0f  };
    c[ImGuiCol_SliderGrabActive]     = { 0.1f,  0.85f, 0.65f, 1.0f  };
    c[ImGuiCol_MenuBarBg]            = { 0,     0,     0,     0     };
    c[ImGuiCol_TitleBg]              = { 0,     0,     0,     0     };
    c[ImGuiCol_TitleBgActive]        = { 0,     0,     0,     0     };
    c[ImGuiCol_Text]                 = { 0.88f, 0.91f, 0.95f, 1.0f  };
    c[ImGuiCol_TextDisabled]         = { 0.32f, 0.38f, 0.48f, 1.0f  };
    c[ImGuiCol_PopupBg]              = { 0.05f, 0.07f, 0.10f, 0.98f };
    c[ImGuiCol_ScrollbarBg]          = { 0,     0,     0,     0     };
    c[ImGuiCol_ScrollbarGrab]        = { 1.0f,  1.0f,  1.0f,  0.10f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 1.0f,  1.0f,  1.0f,  0.18f };
    c[ImGuiCol_ScrollbarGrabActive]  = { 1.0f,  1.0f,  1.0f,  0.28f };
    c[ImGuiCol_Separator]            = { 1.0f,  1.0f,  1.0f,  0.05f };
}


bool RootTool::ActionButton(const char* label, bool enabled, float w, float h, int slot)
{
    static const ImVec4 kStripes[8] = {
        { 1.0f,  0.45f, 0.20f, 1.0f },
        { 1.0f,  0.78f, 0.18f, 1.0f },
        { 0.0f,  0.784f,0.588f,1.0f },
        { 0.0f,  0.784f,0.588f,1.0f },
        { 0.22f, 0.90f, 0.48f, 1.0f },
        { 0.95f, 0.25f, 0.25f, 1.0f },
        { 0.65f, 0.30f, 0.95f, 1.0f },
        { 0.95f, 0.50f, 0.15f, 1.0f },
    };

    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      cursor = ImGui::GetCursorScreenPos();
    float&      glow   = m_btnHover[slot];
    const ImVec4& sc = kStripes[slot < 8 ? slot : 0];

    bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hovered = enabled && ImGui::IsItemHovered();
    bool active  = enabled && ImGui::IsItemActive();

    // Smoother hover glow
    float target = hovered ? 1.0f : 0.0f;
    glow = Lerp(glow, target, ImGui::GetIO().DeltaTime * 10.0f);

    float alpha = enabled ? 1.0f : 0.35f;
    ImColor bg  = active ? ImColor(28, 38, 54, 255) : (hovered ? ImColor(20, 28, 40, 255) : ImColor(15, 20, 28, 255));
    
    dl->AddRectFilled(cursor, { cursor.x + w, cursor.y + h }, bg, 6.0f);
    dl->AddRect(cursor, { cursor.x + w, cursor.y + h }, 
        ImColor(35, 50, 75, (int)(alpha * (50 + glow * 150))), 6.0f, 0, 1.5f);

    // Flat industrial stripe
    float sa = enabled ? (0.4f + glow * 0.6f) : 0.2f;
    dl->AddRectFilled({ cursor.x, cursor.y + 12.0f }, { cursor.x + 2.5f, cursor.y + h - 12.0f }, 
        ImColor(sc.x, sc.y, sc.z, sa));
    
    if (glow > 0.01f) {
        dl->AddRectFilled({ cursor.x, cursor.y + 10.0f }, { cursor.x + 4.0f, cursor.y + h - 10.0f }, 
            ImColor(sc.x, sc.y, sc.z, 0.2f * glow));
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp = { cursor.x + (w - ts.x) * 0.5f, cursor.y + (h - ts.y) * 0.5f };
    dl->AddText(tp, ImColor(200, 220, 240, enabled ? (int)(180 + glow * 75) : 100), label);

    return clicked;
}


void RootTool::RenderUI()
{
    auto  now = std::chrono::steady_clock::now();
    float dt  = m_firstFrame ? 0.016f
        : (std::min)(std::chrono::duration<float>(now - m_lastFrameTime).count(), 0.1f);
    m_firstFrame    = false;
    m_lastFrameTime = now;

    if (m_hasPending.exchange(false)) {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        m_statusMsg     = m_pendingMsg;
        m_statusIsError = m_pendingIsError;
        m_statusAlpha   = 0.0f;
    }

    m_spinnerAngle  = fmodf(m_spinnerAngle + dt * 2.8f, 2.0f * kPI);
    m_logoAlpha     = Clamp01(m_logoAlpha   + dt * kAnimSpeed);
    m_contentAlpha  = Clamp01(m_contentAlpha + dt * kAnimSpeed * 0.8f);
    m_contentSlideY = Lerp(m_contentSlideY, 0.0f, Clamp01(dt * kAnimSpeed));

    if (m_statusMsg != m_prevStatusMsg) { m_statusAlpha = 0.0f; m_prevStatusMsg = m_statusMsg; }
    m_statusAlpha = Clamp01(m_statusAlpha + dt * kAnimSpeed * 1.2f);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##root_main", nullptr, kFlags);

    ImVec2      pos  = ImGui::GetWindowPos();
    ImVec2      size = ImGui::GetWindowSize();
    ImDrawList* dl   = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, ImColor(10, 12, 16, 238), kCornerR);
    dl->AddRect(pos, { pos.x + size.x, pos.y + size.y }, ImColor(20, 26, 36, 255), kCornerR, 0, 1.0f);

    // Header
    {
        ImVec2 hMin = pos;
        ImVec2 hMax = { pos.x + size.x, pos.y + kTitleH };
        dl->AddRectFilled(hMin, { hMax.x, hMax.y }, ImColor(7, 9, 13, 255), kCornerR);
        dl->AddRectFilled({ hMin.x, hMin.y + kCornerR }, hMax, ImColor(7, 9, 13, 255));
        dl->AddLine({ hMin.x + kCornerR, hMax.y }, { hMax.x - kCornerR, hMax.y }, ImColor(18, 24, 34, 255), 1.0f);

        // branding accent
        dl->AddRectFilled({ pos.x + 14, pos.y + 13 }, { pos.x + 17, pos.y + kTitleH - 13 }, ImColor(0, 200, 150, 200));

        float tx = pos.x + 30.0f;
        dl->AddText({ tx, pos.y + 10 }, ImColor(215, 225, 240, 230), "TXC ROOTER");
        dl->AddText({ tx, pos.y + 28 }, ImColor(55, 75, 100, 210), "BlueStacks / MSI Root Tool");

        if (m_logoAlpha < 1.0f) {
            float fade = 1.0f - m_logoAlpha;
            float scx = tx + ImGui::CalcTextSize("TXC ROOTER").x + 15.0f;
            float scy = pos.y + 19.0f;
            float sr = 6.0f;
            int segs = 24; float sweep = kPI * 1.5f, step = sweep / segs;
            for (int i = 0; i < segs; i++) {
                float a0 = m_spinnerAngle + step * i, a1 = a0 + step;
                float t = 1.0f - (float)i / segs;
                dl->AddLine({ scx + cosf(a0) * sr, scy + sinf(a0) * sr }, { scx + cosf(a1) * sr, scy + sinf(a1) * sr }, ImColor(0.0f, 0.784f, 0.588f, t * fade), 1.5f);
            }
        }

        // Window controls
        extern HWND g_hWnd;
        float btnSz = 24.0f, hs = 4.0f;
        float btnY  = pos.y + (kTitleH - btnSz) * 0.5f;
        float right = pos.x + size.x - 12.0f;

        ImVec2 clMin = { right - btnSz, btnY }, clMax = { right, btnY + btnSz };
        if (ImGui::IsMouseHoveringRect(clMin, clMax)) {
            dl->AddRectFilled(clMin, clMax, ImColor(210, 40, 40, 230), 4.0f);
            if (ImGui::IsMouseClicked(0)) ::PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        } else {
            dl->AddRectFilled(clMin, clMax, ImColor(255, 255, 255, 10), 4.0f);
        }
        float cx = (clMin.x + clMax.x) * 0.5f, cy = (clMin.y + clMax.y) * 0.5f;
        dl->AddLine({ cx - hs, cy - hs }, { cx + hs, cy + hs }, ImColor(255, 255, 255, 200), 1.5f);
        dl->AddLine({ cx + hs, cy - hs }, { cx - hs, cy + hs }, ImColor(255, 255, 255, 200), 1.5f);

        ImVec2 mnMin = { right - btnSz * 2 - 5, btnY }, mnMax = { right - btnSz - 5, btnY + btnSz };
        if (ImGui::IsMouseHoveringRect(mnMin, mnMax)) {
            dl->AddRectFilled(mnMin, mnMax, ImColor(255, 255, 255, 25), 4.0f);
            if (ImGui::IsMouseClicked(0)) ::ShowWindow(g_hWnd, SW_MINIMIZE);
        } else {
            dl->AddRectFilled(mnMin, mnMax, ImColor(255, 255, 255, 10), 4.0f);
        }
        float mx = (mnMin.x + mnMax.x) * 0.5f, my = (mnMin.y + mnMax.y) * 0.5f;
        dl->AddLine({ mx - hs, my }, { mx + hs, my }, ImColor(255, 255, 255, 200), 1.5f);
    }

    const float pad      = 22.0f;
    const float contentX = pad;
    const float contentW = size.x - pad * 2.0f;
    const float contentY = kTitleH + 14.0f + m_contentSlideY;

    ImGui::SetCursorPos({ contentX, contentY });
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_contentAlpha);
    ImGui::BeginGroup();

    // -- Emulator selector
    {
        bool bstk = (m_selectedEmulator == 0);
        auto RadioPill = [&](const char* lbl, bool active, int id) -> bool {
            ImVec2 cs = ImGui::GetCursorScreenPos();
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            float  pw = ts.x + 22.0f, ph = ts.y + 10.0f;
            std::string btnId = std::string("##rp") + std::to_string(id);
            ImGui::InvisibleButton(btnId.c_str(), { pw, ph });
            bool clicked = ImGui::IsItemClicked();
            bool hov     = ImGui::IsItemHovered();
            ImColor bg     = active ? ImColor(0, 200, 150, 38)  : (hov ? ImColor(255, 255, 255, 10) : ImColor(0, 0, 0, 0));
            ImColor border = active ? ImColor(0, 200, 150, 150) : ImColor(35, 48, 65, 255);
            dl->AddRectFilled(cs, { cs.x + pw, cs.y + ph }, bg, 20.0f);
            dl->AddRect(cs, { cs.x + pw, cs.y + ph }, border, 20.0f, 0, 1.0f);
            dl->AddText({ cs.x + 11, cs.y + 5 }, active ? ImColor(0, 200, 150, 255) : ImColor(130, 155, 185, 255), lbl);
            return clicked;
        };

        ImGui::TextColored({ 0.28f, 0.38f, 0.52f, 1.0f }, "EMULATOR");
        ImGui::SameLine(100.0f);
        if (RadioPill("BlueStacks 5",   bstk,  0)) { m_selectedEmulator = 0; m_selectedInstance.clear(); RefreshEmulatorInfo(); }
        ImGui::SameLine(0, 8.0f);
        if (RadioPill("MSI App Player", !bstk, 1)) { m_selectedEmulator = 1; m_selectedInstance.clear(); RefreshEmulatorInfo(); }
    }

    EmulatorInfo& emu = m_selectedEmulator == 0 ? m_bluestacks : m_msi;
    ImGui::Spacing();

    // -- Info cards
    {
        float cardW = (contentW - 8.0f) * 0.5f;
        auto InfoCard = [&](const char* label, const char* value) {
            ImVec2 cs = ImGui::GetCursorScreenPos();
            float  ch = 52.0f;
            ImGui::Dummy({ cardW, ch });
            dl->AddRectFilled(cs, { cs.x + cardW, cs.y + ch }, ImColor(13, 17, 23, 255), 6.0f);
            dl->AddRect(cs, { cs.x + cardW, cs.y + ch }, ImColor(28, 38, 55, 255), 6.0f, 0, 1.0f);
            dl->AddRectFilled({ cs.x, cs.y + 14.0f }, { cs.x + 2.5f, cs.y + ch - 14.0f }, ImColor(0, 200, 150, 180));
            dl->AddText({ cs.x + 12, cs.y + 7 }, ImColor(65, 85, 115, 255), label);
            const char* v = (value && value[0]) ? value : "(not found)";
            float lineH = ImGui::GetTextLineHeight();
            dl->PushClipRect({ cs.x + 12, cs.y + ch - lineH - 8 }, { cs.x + cardW - 8, cs.y + ch }, true);
            dl->AddText({ cs.x + 12, cs.y + ch - lineH - 8 }, ImColor(185, 205, 230, 240), v);
            dl->PopClipRect();
        };

        InfoCard("INSTALL DIR", emu.installDir.c_str());
        ImGui::SameLine(0, 8.0f);
        InfoCard("DATA DIR", emu.dataDir.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // -- Instance selection
    {
        if (!emu.instances.empty() && m_selectedInstance.empty())
            m_selectedInstance = emu.instances[0].instanceName;

        std::string preview = "(none found)";
        for (const auto& inst : emu.instances)
            if (inst.instanceName == m_selectedInstance)
                { preview = inst.displayName + " (" + inst.instanceName + ")"; break; }
        if (preview == "(none found)" && !m_selectedInstance.empty())
            preview = m_selectedInstance;

        ImGui::TextColored({ 0.28f, 0.38f, 0.52f, 1.0f }, "INSTANCE");
        ImGui::SameLine(100.0f);

        float dropW    = contentW * 0.55f;
        ImVec2 dropPos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Button,        { 0.05f, 0.07f, 0.10f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.08f, 0.11f, 0.16f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  { 0.10f, 0.14f, 0.20f, 1.0f });
        if (ImGui::Button((preview + "##dd").c_str(), { dropW, ImGui::GetFrameHeight() }))
            m_showInstanceList = !m_showInstanceList;
        ImGui::PopStyleColor(3);

        float ax = dropPos.x + dropW - 15, ay = dropPos.y + ImGui::GetFrameHeight() * 0.5f - 3.0f;
        if (m_showInstanceList)
            dl->AddTriangleFilled({ ax, ay + 5 }, { ax + 8, ay + 5 }, { ax + 4, ay - 1 }, ImColor(90, 120, 160, 200));
        else
            dl->AddTriangleFilled({ ax, ay }, { ax + 8, ay }, { ax + 4, ay + 6 }, ImColor(90, 120, 160, 200));

        if (!m_selectedInstance.empty()) {
            ImGui::SameLine(0, 12.0f);
            ImVec2 bp = ImGui::GetCursorScreenPos();
            bool master = IsMasterInstance(m_selectedInstance);
            const char* blbl = master ? "MASTER" : "CLONE";
            ImVec2 bts = ImGui::CalcTextSize(blbl);
            float bw = bts.x + 14.0f, bh = bts.y + 4.0f;
            ImGui::Dummy({ bw, bh });
            ImColor bcol = master ? ImColor(0, 200, 150, 45) : ImColor(255, 140, 40, 45);
            ImColor btxt = master ? ImColor(0, 200, 150, 255) : ImColor(255, 140, 40, 255);
            dl->AddRectFilled(bp, { bp.x + bw, bp.y + bh }, bcol, 4.0f);
            dl->AddRect(bp, { bp.x + bw, bp.y + bh }, ImColor(btxt.Value.x, btxt.Value.y, btxt.Value.z, 0.4f), 4.0f, 0, 1.0f);
            dl->AddText({ bp.x + 7, bp.y + 2 }, btxt, blbl);
        }

        if (m_showInstanceList && !emu.instances.empty()) {
            int   maxVis = (std::min)((int)emu.instances.size(), 5);
            float listH  = ImGui::GetTextLineHeightWithSpacing() * maxVis + ImGui::GetStyle().FramePadding.y * 2;
            if (ImGui::BeginListBox("##instlist", { dropW, listH })) {
                for (const auto& inst : emu.instances) {
                    bool sel = inst.instanceName == m_selectedInstance;
                    std::string lbl = inst.displayName + " (" + inst.instanceName + ")##" + inst.instanceName;
                    if (ImGui::Selectable(lbl.c_str(), sel)) { m_selectedInstance = inst.instanceName; m_showInstanceList = false; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndListBox();
            }
        }
    }

    ImGui::Spacing();
    dl->AddLine({ pos.x + pad, ImGui::GetCursorScreenPos().y }, { pos.x + size.x - pad, ImGui::GetCursorScreenPos().y }, ImColor(18, 26, 38, 255));
    ImGui::Spacing();

    {
        const bool  busy     = m_busy.load();
        const bool  canAct   = emu.found() && !busy;
        const bool  hasInst  = !m_selectedInstance.empty();
        const float sp       = 8.0f;
        const float btnW     = (contentW - sp) * 0.5f;
        std::string master   = GetMasterInstanceName(m_selectedInstance);

        if (ActionButton("Kill Emulator Processes", !busy,          btnW, kBtnH, 0)) {
            std::string inst = m_selectedInstance;
            RunAsync([this] { KillProcesses(); });
        }
        ImGui::SameLine(0, sp);
        if (ActionButton("Fix Illegally Tampered",  canAct,         btnW, kBtnH, 1)) {
            std::string dir = emu.installDir;
            RunAsync([this, dir] { PatchHDPlayer(dir); });
        }

        ImGui::Spacing();

        if (ActionButton("Disk  R/W", canAct && hasInst, btnW, kBtnH, 2)) {
            std::string d = emu.dataDir, m = master;
            RunAsync([this, d, m] { ApplyRootConfigs(d, m); });
        }
        ImGui::SameLine(0, sp);
        if (ActionButton("Disk  R/O", canAct && hasInst, btnW, kBtnH, 3)) {
            std::string d = emu.dataDir, m = master;
            RunAsync([this, d, m] { RevertDiskToReadonly(d, m); });
        }

        ImGui::Spacing();

        if (ActionButton("One Click Root",   canAct && hasInst, btnW, kBtnH, 4)) {
            std::string d = emu.dataDir, inst = m_selectedInstance;
            RunAsync([this, d, inst] { OneClickRoot(d, inst); });
        }
        ImGui::SameLine(0, sp);
        if (ActionButton("One Click Unroot", canAct && hasInst, btnW, kBtnH, 5)) {
            std::string d = emu.dataDir, inst = m_selectedInstance;
            RunAsync([this, d, inst] { OneClickUnroot(d, inst); });
        }

        ImGui::Spacing();

        if (ActionButton("Install SuperSU",   canAct && hasInst, btnW, kBtnH, 6)) {
            std::string idir = emu.installDir, d = emu.dataDir, inst = m_selectedInstance;
            RunAsync([this, idir, d, inst] { InstallSuperSU(idir, d, inst); });
        }
        ImGui::SameLine(0, sp);
        if (ActionButton("Uninstall SuperSU", canAct && hasInst, btnW, kBtnH, 7)) {
            std::string d = emu.dataDir, inst = m_selectedInstance;
            RunAsync([this, d, inst] { UninstallSuperSU(d, inst); });
        }

        if (busy) {
            ImGui::Spacing();
            ImGui::SetCursorPosX((contentW - ImGui::CalcTextSize("Working...").x) * 0.5f + pad);
            ImGui::TextColored({ 0.0f, 0.784f, 0.588f, 0.7f + 0.3f * sinf(m_spinnerAngle * 3.0f) }, "Working...");
        }
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar();

    // Status Bar
    {
        float sbY = pos.y + size.y - kStatusBarH;
        dl->AddLine({ pos.x + pad, sbY }, { pos.x + size.x - pad, sbY }, ImColor(22, 32, 45, 255), 1.0f);
        float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorPos({ pad + 8.0f, size.y - kStatusBarH + (kStatusBarH - th) * 0.5f });
        if (!m_statusMsg.empty()) {
            bool   err = m_statusIsError;
            ImVec4 col = err ? kError : kAccent;
            ImGui::TextColored(ImVec4(col.x, col.y, col.z, m_statusAlpha), err ? "[!]  %s" : "[OK]  %s", m_statusMsg.c_str());
        } else {
            ImGui::TextDisabled("Idle - System ready for operations.");
        }
    }

    ImGui::End();

    for (int i = 0; i < 8; i++)
        m_btnHover[i] = Clamp01(m_btnHover[i] - dt * kHoverSpeed);
}


void RootTool::InstallSuperSU(const std::string& installDir, const std::string& dataDir, const std::string& selectedInstance)
{
    if (selectedInstance.empty()) return;

    HRSRC   hRes  = FindResource(nullptr, MAKEINTRESOURCE(IDR_SUPERSU_APK), RT_RCDATA);
    HGLOBAL hData = hRes ? LoadResource(nullptr, hRes) : nullptr;
    DWORD   apkSz = hRes ? SizeofResource(nullptr, hRes) : 0;
    const uint8_t* apkRaw = hData ? (const uint8_t*)LockResource(hData) : nullptr;
    if (!apkRaw || !apkSz) { SetStatus("Internal error: SuperSU APK resource missing.", true); return; }

    char tempDir[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, tempDir);
    std::string tmpApk = std::string(tempDir) + "SuperSU.apk";
    {
        std::ofstream tmp(tmpApk, std::ios::binary);
        if (!tmp) { SetStatus("Failed to write APK to temp.", true); return; }
        tmp.write(reinterpret_cast<const char*>(apkRaw), apkSz);
    }

    std::string adbPath;
    auto tryAdb = [&](const std::string& p) { if (!p.empty() && fs::exists(p)) adbPath = p; };
    tryAdb(installDir + "HD-Adb.exe");
    if (adbPath.empty()) {
        char buf[MAX_PATH]{};
        if (SearchPathA(nullptr, "adb.exe", nullptr, MAX_PATH, buf, nullptr))
            adbPath = buf;
    }
    if (adbPath.empty()) {
        fs::remove(tmpApk);
        SetStatus("ADB not found. Ensure emulator is running or add ADB to PATH.", true);
        return;
    }

    SetStatus("Installing SuperSU via ADB...", false);

    std::string cmd = "\"" + adbPath + "\" install -r \"" + tmpApk + "\"";
    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        fs::remove(tmpApk);
        SetStatus("Failed to launch ADB process.", true);
        return;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    fs::remove(tmpApk);

    if (exitCode == 0)
        SetStatus("SuperSU installed! Open the app in the emulator to finish setup.", false);
    else
        SetStatus("ADB install failed — make sure the emulator is running.", true);
}

void RootTool::UninstallSuperSU(const std::string& dataDir, const std::string& selectedInstance)
{
    if (dataDir.empty() || selectedInstance.empty()) return;

    std::string masterInst = GetMasterInstanceName(selectedInstance);
    KillProcesses();
    ::Sleep(1000);

    std::string engineDir = dataDir;
    if (engineDir.back() != '\\' && engineDir.back() != '/') engineDir += '\\';
    std::string vhdPath = engineDir + masterInst + "\\Root.vhd";
    if (!fs::exists(vhdPath)) { SetStatus("Root.vhd not found.", true); return; }

    VHDManager vhd;
    if (!vhd.OpenVHD(vhdPath)) { SetStatus("Failed to open VHD: " + vhd.GetLastError(), true); return; }

    int ext4Idx = -1;
    for (size_t i = 0; i < vhd.GetPartitions().size(); i++)
        if (vhd.GetPartitions()[i].is_ext4 && ext4Idx < 0) ext4Idx = (int)i;

    if (ext4Idx < 0) { SetStatus("No ext4 partition found.", true); vhd.CloseVHD(); return; }

    if (!vhd.MountExt4Partition(ext4Idx)) {
        SetStatus("Failed to mount ext4: " + vhd.GetLastError(), true);
        vhd.CloseVHD(); return;
    }

    bool removed = false;
    const std::string targets[] = {
        "/android/data/app/SuperSU.apk",
        "/android/system/app/SuperSU/SuperSU.apk",
        "/android/system/priv-app/SuperSU/SuperSU.apk",
        "/android/system/xbin/su",
    };
    for (const auto& p : targets) {
        if (vhd.FileExists(p)) { vhd.DeleteFile(p); removed = true; }
    }

    vhd.UnmountExt4();
    vhd.CloseVHD();
    SetStatus(removed ? "SuperSU removed successfully." : "SuperSU files not found — already clean.", false);
}



