#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>


struct BstkInstance {
    std::string displayName;
    std::string instanceName;
};

struct EmulatorInfo {
    bool        isBlueStacks = true;
    std::string name;
    std::string installDir;
    std::string dataDir;
    std::vector<BstkInstance> instances;

    bool found() const { return !installDir.empty(); }
};


class RootTool {
public:
    RootTool();

    void SetLogo(void* srv, int w, int h) {
        m_logoTexture = srv;
        m_logoWidth   = w;
        m_logoHeight  = h;
    }

    static void SetupTheme();
    void RenderUI();

private:
    void RefreshEmulatorInfo();
    void FindInstances(EmulatorInfo& info);

    void KillProcesses();
    void PatchHDPlayer(const std::string& installDir);
    void ApplyRootConfigs(const std::string& dataDir, const std::string& instanceName);
    void RevertDiskToReadonly(const std::string& dataDir, const std::string& instanceName);
    void OneClickRoot(const std::string& dataDir, const std::string& instanceName);
    void OneClickUnroot(const std::string& dataDir, const std::string& instanceName);
    void InstallSuperSU(const std::string& installDir, const std::string& dataDir, const std::string& instanceName);
    void UninstallSuperSU(const std::string& dataDir, const std::string& instanceName);

    void RunAsync(std::function<void()> fn);

    std::string ReadRegistryString(const std::string& subKey, const std::string& valueName);
    std::string ReadFileString(const std::string& path);
    bool        WriteFileString(const std::string& path, const std::string& content);
    void        Log(const std::string& msg, bool isError = false);
    void        SetStatus(const std::string& msg, bool isError);
    bool        IsMasterInstance(const std::string& instanceName);
    std::string GetMasterInstanceName(const std::string& instanceName);
    bool        ActionButton(const char* label, bool enabled, float w, float h, int slot);

    EmulatorInfo m_bluestacks;
    EmulatorInfo m_msi;
    int          m_selectedEmulator = 0;
    std::string  m_selectedInstance;
    std::string  m_statusMsg;
    bool         m_statusIsError = false;

    void* m_logoTexture = nullptr;
    int   m_logoWidth   = 0;
    int   m_logoHeight  = 0;
    bool  m_showInstanceList = false;

    float m_spinnerAngle  = 0.0f;
    float m_logoAlpha     = 0.0f;
    float m_contentAlpha  = 0.0f;
    float m_contentSlideY = 8.0f;
    float m_statusAlpha   = 0.0f;
    std::string m_prevStatusMsg;

    float m_btnHover[8] = {};

    std::chrono::steady_clock::time_point m_lastFrameTime;
    bool m_firstFrame = true;

    std::atomic<bool> m_busy{ false };

    std::mutex  m_pendingMtx;
    std::string m_pendingMsg;
    bool        m_pendingIsError  = false;
    std::atomic<bool> m_hasPending{ false };
};
