/* RA_Interface_posix.cpp - the RetroAchievements client interface off Windows.
 *
 * The counterpart of RA_Interface.cpp, which loads RA_Integration.dll on
 * Windows. An emulator builds exactly one of the two; both implement
 * RA_Interface.h.
 *
 * It loads libRA_Integration.so from the directory of the running executable,
 * as RA_Interface.cpp loads the DLL from beside the emulator's .exe, and the
 * toolkit keeps its RACache directory in that same place. There is no
 * download and no version check: the server's r=latestintegration answer
 * only ever offers the Windows DLL. If the library is missing or cannot be
 * loaded, one line goes to stderr and every RA_* function below does nothing,
 * so the emulator runs without achievements. The executable's directory is
 * found through /proc/self/exe, which Linux provides; where that is missing
 * the lookup fails and the emulator runs without achievements.
 *
 * The library is loaded RTLD_LOCAL, which keeps its symbols out of the
 * process's global scope: nothing loaded after it can bind to them. That
 * does not stop the library's own references from resolving against
 * definitions already in the process; libRA_Integration.so is linked with
 * -Bsymbolic-functions so that its calls to its own functions stay inside
 * it.
 *
 * Every definition must match its declaration in RA_Interface.h exactly,
 * spelled with the header's RA_WindowHandle and RA_MenuItemId. A definition
 * with different parameter types is not a compile error: the declarations are
 * extern "C", a differently-typed definition is just another C++ function,
 * and the C symbol the emulator links against quietly goes missing.
 */

#ifdef _WIN32
 #error "RA_Interface_posix.cpp is the non-Windows loader; Windows emulators build RA_Interface.cpp"
#endif

#include "RA_Interface.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#define RA_INT_SO "libRA_Integration.so"

/* The library's entry points. Names, order and types follow the table in
 * RA_Interface.cpp, with HWND, LPARAM and BYTE spelled RA_WindowHandle,
 * RA_MenuItemId and unsigned char, and without the Win32-only
 * _RA_CreatePopupMenu. */

// Initialization
static const char*  (*_RA_IntegrationVersion)() = nullptr;
static const char*  (*_RA_HostName)() = nullptr;
static const char*  (*_RA_HostUrl)() = nullptr;
static int          (*_RA_InitI)(RA_WindowHandle hMainWnd, int nConsoleID, const char* sClientVer) = nullptr;
static int          (*_RA_InitOffline)(RA_WindowHandle hMainWnd, int nConsoleID, const char* sClientVer) = nullptr;
static int          (*_RA_InitClient)(RA_WindowHandle hMainWnd, const char* sClientName, const char* sClientVer) = nullptr;
static int          (*_RA_InitClientOffline)(RA_WindowHandle hMainWnd, const char* sClientName, const char* sClientVer) = nullptr;
static void         (*_RA_InstallSharedFunctions)(int(*)(), void(*)(), void(*)(), void(*)(), void(*)(char*), void(*)(), void(*)(const char*)) = nullptr;
static void         (*_RA_SetForceRepaint)(int bEnable) = nullptr;
static int          (*_RA_GetPopupMenuItems)(RA_MenuItem*) = nullptr;
static void         (*_RA_InvokeDialog)(RA_MenuItemId nID) = nullptr;
static void         (*_RA_SetUserAgentDetail)(const char* sDetail) = nullptr;
static void         (*_RA_AttemptLogin)(int bBlocking) = nullptr;
static int          (*_RA_SetConsoleID)(unsigned int nConsoleID) = nullptr;
static void         (*_RA_ClearMemoryBanks)() = nullptr;
static void         (*_RA_InstallMemoryBank)(int nBankID, RA_ReadMemoryFunc* pReader, RA_WriteMemoryFunc* pWriter, int nBankSize) = nullptr;
static void         (*_RA_InstallMemoryBankBlockReader)(int nBankID, RA_ReadMemoryBlockFunc* pReader) = nullptr;
static int          (*_RA_Shutdown)() = nullptr;
// Overlay
static int          (*_RA_IsOverlayFullyVisible)() = nullptr;
static void         (*_RA_SetPaused)(int bIsPaused) = nullptr;
static void         (*_RA_NavigateOverlay)(ControllerInput* pInput) = nullptr;
static void         (*_RA_UpdateHWnd)(RA_WindowHandle hMainHWND) = nullptr;
// Game Management
static unsigned int (*_RA_IdentifyRom)(const unsigned char* pROM, unsigned int nROMSize) = nullptr;
static unsigned int (*_RA_IdentifyHash)(const char* sHash) = nullptr;
static void         (*_RA_ActivateGame)(unsigned int nGameId) = nullptr;
static int          (*_RA_OnLoadNewRom)(const unsigned char* pROM, unsigned int nROMSize) = nullptr;
static int          (*_RA_ConfirmLoadNewRom)(int bQuitting) = nullptr;
// Runtime Functionality
static void         (*_RA_DoAchievementsFrame)() = nullptr;
static void         (*_RA_SuspendRepaint)() = nullptr;
static void         (*_RA_ResumeRepaint)() = nullptr;
static void         (*_RA_UpdateAppTitle)(const char* pMessage) = nullptr;
static const char*  (*_RA_UserName)() = nullptr;
static int          (*_RA_HardcoreModeIsActive)(void) = nullptr;
static int          (*_RA_WarnDisableHardcore)(const char* sActivity) = nullptr;
static void         (*_RA_OnReset)() = nullptr;
static void         (*_RA_OnSaveState)(const char* sFilename) = nullptr;
static void         (*_RA_OnLoadState)(const char* sFilename) = nullptr;
static int          (*_RA_CaptureState)(char* pBuffer, int nBufferSize) = nullptr;
static void         (*_RA_RestoreState)(const char* pBuffer) = nullptr;

static void* g_hRAIntegration = nullptr;

/* The full path libRA_Integration.so was (or was last) loaded from, set by
   InstallIntegration and used in every message that names the library. */
static std::string g_sIntegrationPath;

void RA_AttemptLogin(int bBlocking)
{
    if (_RA_AttemptLogin != nullptr)
        _RA_AttemptLogin(bBlocking);
}

const char* RA_UserName(void)
{
    if (_RA_UserName != nullptr)
        return _RA_UserName();

    return "";
}

void RA_NavigateOverlay(ControllerInput* pInput)
{
    if (_RA_NavigateOverlay != nullptr)
        _RA_NavigateOverlay(pInput);
}

int RA_IsOverlayFullyVisible(void)
{
    if (_RA_IsOverlayFullyVisible != nullptr)
        return _RA_IsOverlayFullyVisible();

    return 0;
}

void RA_UpdateHWnd(RA_WindowHandle hMainWnd)
{
    if (_RA_UpdateHWnd != nullptr)
        _RA_UpdateHWnd(hMainWnd);
}

unsigned int RA_IdentifyRom(unsigned char* pROMData, unsigned int nROMSize)
{
    if (_RA_IdentifyRom != nullptr)
        return _RA_IdentifyRom(pROMData, nROMSize);

    return 0;
}

unsigned int RA_IdentifyHash(const char* sHash)
{
    if (_RA_IdentifyHash != nullptr)
        return _RA_IdentifyHash(sHash);

    return 0;
}

void RA_ActivateGame(unsigned int nGameId)
{
    if (_RA_ActivateGame != nullptr)
        _RA_ActivateGame(nGameId);
}

void RA_OnLoadNewRom(unsigned char* pROMData, unsigned int nROMSize)
{
    if (_RA_OnLoadNewRom != nullptr)
        _RA_OnLoadNewRom(pROMData, nROMSize);
}

void RA_ClearMemoryBanks(void)
{
    if (_RA_ClearMemoryBanks != nullptr)
        _RA_ClearMemoryBanks();
}

void RA_InstallMemoryBank(int nBankID, RA_ReadMemoryFunc pReader, RA_WriteMemoryFunc pWriter, int nBankSize)
{
    if (_RA_InstallMemoryBank != nullptr)
        _RA_InstallMemoryBank(nBankID, pReader, pWriter, nBankSize);
}

void RA_InstallMemoryBankBlockReader(int nBankID, RA_ReadMemoryBlockFunc pReader)
{
    if (_RA_InstallMemoryBankBlockReader != nullptr)
        _RA_InstallMemoryBankBlockReader(nBankID, pReader);
}

int RA_GetPopupMenuItems(RA_MenuItem *pItems)
{
    return (_RA_GetPopupMenuItems != nullptr) ? _RA_GetPopupMenuItems(pItems) : 0;
}

void RA_UpdateAppTitle(const char* sCustomMsg)
{
    if (_RA_UpdateAppTitle != nullptr)
        _RA_UpdateAppTitle(sCustomMsg);
}

void RA_HandleHTTPResults(void)
{
}

int RA_ConfirmLoadNewRom(int bIsQuitting)
{
    return _RA_ConfirmLoadNewRom ? _RA_ConfirmLoadNewRom(bIsQuitting) : 1;
}

void RA_InvokeDialog(RA_MenuItemId nID)
{
    if (_RA_InvokeDialog != nullptr)
        _RA_InvokeDialog(nID);
}

void RA_SetPaused(bool bIsPaused)
{
    if (_RA_SetPaused != nullptr)
        _RA_SetPaused(bIsPaused);
}

void RA_OnLoadState(const char* sFilename)
{
    if (_RA_OnLoadState != nullptr)
        _RA_OnLoadState(sFilename);
}

void RA_OnSaveState(const char* sFilename)
{
    if (_RA_OnSaveState != nullptr)
        _RA_OnSaveState(sFilename);
}

int RA_CaptureState(char* pBuffer, int nBufferSize)
{
    if (_RA_CaptureState != nullptr)
        return _RA_CaptureState(pBuffer, nBufferSize);

    return 0;
}

void RA_RestoreState(const char* pBuffer)
{
    if (_RA_RestoreState != nullptr)
        _RA_RestoreState(pBuffer);
}

void RA_OnReset(void)
{
    if (_RA_OnReset != nullptr)
        _RA_OnReset();
}

void RA_DoAchievementsFrame(void)
{
    if (_RA_DoAchievementsFrame != nullptr)
        _RA_DoAchievementsFrame();
}

void RA_SetForceRepaint(int bEnable)
{
    if (_RA_SetForceRepaint != nullptr)
        _RA_SetForceRepaint(bEnable);
}

void RA_SuspendRepaint(void)
{
    if (_RA_SuspendRepaint != nullptr)
        _RA_SuspendRepaint();
}

void RA_ResumeRepaint(void)
{
    if (_RA_ResumeRepaint != nullptr)
        _RA_ResumeRepaint();
}

void RA_SetConsoleID(unsigned int nConsoleID)
{
    if (_RA_SetConsoleID != nullptr)
        _RA_SetConsoleID(nConsoleID);
}

int RA_HardcoreModeIsActive(void)
{
    return (_RA_HardcoreModeIsActive != nullptr) ? _RA_HardcoreModeIsActive() : 0;
}

int RA_WarnDisableHardcore(const char* sActivity)
{
    // If Hardcore mode not active, allow the activity.
    if (!RA_HardcoreModeIsActive())
        return 1;

    // DLL function will display a yes/no dialog. If the user chooses yes, the DLL will disable hardcore mode, and the activity can proceed.
    if (_RA_WarnDisableHardcore != nullptr)
        return _RA_WarnDisableHardcore(sActivity);

    // We cannot disable hardcore mode, so just warn the user and prevent the activity.
    // RA_Interface.cpp shows this in a message box; there is no toolkit to show one with here.
    std::fprintf(stderr, "RA_Interface: You cannot %s while Hardcore mode is active.\n",
                 sActivity ? sActivity : "do that");
    return 0;
}

void RA_DisableHardcore(void)
{
    // passing nullptr to _RA_WarnDisableHardcore will just disable hardcore mode without prompting.
    if (_RA_WarnDisableHardcore != nullptr)
        _RA_WarnDisableHardcore(nullptr);
}

/* The directory of the running executable, ending in '/', or an empty string
 * if it cannot be read. The counterpart of GetIntegrationPath() in
 * RA_Interface.cpp, which uses GetModuleFileNameW(0). */
static std::string GetExecutableDirectory()
{
    char sBuffer[4096];
    const ssize_t nLength = readlink("/proc/self/exe", sBuffer, sizeof(sBuffer));

    /* readlink does not terminate the string, and fills the whole buffer when
       it had to truncate the path */
    if (nLength <= 0 || nLength >= static_cast<ssize_t>(sizeof(sBuffer)))
        return std::string();

    const std::string sPath(sBuffer, static_cast<size_t>(nLength));
    const size_t nSlash = sPath.rfind('/');
    if (nSlash == std::string::npos)
        return std::string();

    return sPath.substr(0, nSlash + 1);
}

template<typename TFunction>
static void Resolve(TFunction& pFunction, const char* sName)
{
    /* POSIX guarantees that a function's address survives the round trip
       through dlsym's void* */
    pFunction = reinterpret_cast<TFunction>(dlsym(g_hRAIntegration, sName));
}

static void UnloadIntegration()
{
    //	Clear func ptrs
    _RA_IntegrationVersion = nullptr;
    _RA_HostName = nullptr;
    _RA_HostUrl = nullptr;
    _RA_InitI = nullptr;
    _RA_InitOffline = nullptr;
    _RA_InitClient = nullptr;
    _RA_InitClientOffline = nullptr;
    _RA_InstallSharedFunctions = nullptr;
    _RA_SetForceRepaint = nullptr;
    _RA_GetPopupMenuItems = nullptr;
    _RA_InvokeDialog = nullptr;
    _RA_SetUserAgentDetail = nullptr;
    _RA_AttemptLogin = nullptr;
    _RA_SetConsoleID = nullptr;
    _RA_ClearMemoryBanks = nullptr;
    _RA_InstallMemoryBank = nullptr;
    _RA_InstallMemoryBankBlockReader = nullptr;
    _RA_Shutdown = nullptr;
    _RA_IsOverlayFullyVisible = nullptr;
    _RA_SetPaused = nullptr;
    _RA_NavigateOverlay = nullptr;
    _RA_UpdateHWnd = nullptr;
    _RA_IdentifyRom = nullptr;
    _RA_IdentifyHash = nullptr;
    _RA_ActivateGame = nullptr;
    _RA_OnLoadNewRom = nullptr;
    _RA_ConfirmLoadNewRom = nullptr;
    _RA_DoAchievementsFrame = nullptr;
    _RA_SuspendRepaint = nullptr;
    _RA_ResumeRepaint = nullptr;
    _RA_UpdateAppTitle = nullptr;
    _RA_UserName = nullptr;
    _RA_HardcoreModeIsActive = nullptr;
    _RA_WarnDisableHardcore = nullptr;
    _RA_OnReset = nullptr;
    _RA_OnSaveState = nullptr;
    _RA_OnLoadState = nullptr;
    _RA_CaptureState = nullptr;
    _RA_RestoreState = nullptr;

    /* unload the library. A clang build unmaps it here; a GCC build most
       likely stays mapped. Nothing depends on either. */
    if (g_hRAIntegration != nullptr)
    {
        dlclose(g_hRAIntegration);
        g_hRAIntegration = nullptr;
    }
}

/* Loads the library and resolves every entry point. Returns false, having
 * said why on stderr and unloaded anything it loaded, when there is no
 * usable library. A second call reuses the loaded library, as a second
 * LoadLibraryW of the DLL would. */
static bool InstallIntegration()
{
    if (g_hRAIntegration != nullptr)
        return true;

    const std::string sDirectory = GetExecutableDirectory();
    if (sDirectory.empty())
    {
        std::fprintf(stderr, "RA_Interface: " RA_INT_SO " not found: the executable's directory could not be read; achievements are disabled\n");
        return false;
    }

    const std::string sPath = sDirectory + RA_INT_SO;
    struct stat oStat;
    if (stat(sPath.c_str(), &oStat) != 0)
    {
        const int nStatErrno = errno;
        if (nStatErrno == ENOENT)
        {
            std::fprintf(stderr, "RA_Interface: " RA_INT_SO " not found in %s; achievements are disabled\n", sDirectory.c_str());
        }
        else
        {
            std::fprintf(stderr, "RA_Interface: cannot access %s: %s; achievements are disabled\n", sPath.c_str(),
                         std::strerror(nStatErrno));
        }
        return false;
    }

    g_hRAIntegration = dlopen(sPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (g_hRAIntegration == nullptr)
    {
        const char* sError = dlerror();
        std::fprintf(stderr, "RA_Interface: could not load %s: %s; achievements are disabled\n", sPath.c_str(),
                     sError ? sError : "unknown error");
        return false;
    }

    g_sIntegrationPath = sPath;

    //	Install function pointers one by one
    Resolve(_RA_IntegrationVersion, "_RA_IntegrationVersion");
    Resolve(_RA_HostName, "_RA_HostName");
    Resolve(_RA_HostUrl, "_RA_HostUrl");
    Resolve(_RA_InitI, "_RA_InitI");
    Resolve(_RA_InitOffline, "_RA_InitOffline");
    Resolve(_RA_InitClient, "_RA_InitClient");
    Resolve(_RA_InitClientOffline, "_RA_InitClientOffline");
    Resolve(_RA_InstallSharedFunctions, "_RA_InstallSharedFunctionsExt");
    Resolve(_RA_SetForceRepaint, "_RA_SetForceRepaint");
    Resolve(_RA_GetPopupMenuItems, "_RA_GetPopupMenuItems");
    Resolve(_RA_InvokeDialog, "_RA_InvokeDialog");
    Resolve(_RA_SetUserAgentDetail, "_RA_SetUserAgentDetail");
    Resolve(_RA_AttemptLogin, "_RA_AttemptLogin");
    Resolve(_RA_SetConsoleID, "_RA_SetConsoleID");
    Resolve(_RA_ClearMemoryBanks, "_RA_ClearMemoryBanks");
    Resolve(_RA_InstallMemoryBank, "_RA_InstallMemoryBank");
    Resolve(_RA_InstallMemoryBankBlockReader, "_RA_InstallMemoryBankBlockReader");
    Resolve(_RA_Shutdown, "_RA_Shutdown");
    Resolve(_RA_IsOverlayFullyVisible, "_RA_IsOverlayFullyVisible");
    Resolve(_RA_SetPaused, "_RA_SetPaused");
    Resolve(_RA_NavigateOverlay, "_RA_NavigateOverlay");
    Resolve(_RA_UpdateHWnd, "_RA_UpdateHWnd");
    Resolve(_RA_IdentifyRom, "_RA_IdentifyRom");
    Resolve(_RA_IdentifyHash, "_RA_IdentifyHash");
    Resolve(_RA_ActivateGame, "_RA_ActivateGame");
    Resolve(_RA_OnLoadNewRom, "_RA_OnLoadNewRom");
    Resolve(_RA_ConfirmLoadNewRom, "_RA_ConfirmLoadNewRom");
    Resolve(_RA_DoAchievementsFrame, "_RA_DoAchievementsFrame");
    Resolve(_RA_SuspendRepaint, "_RA_SuspendRepaint");
    Resolve(_RA_ResumeRepaint, "_RA_ResumeRepaint");
    Resolve(_RA_UpdateAppTitle, "_RA_UpdateAppTitle");
    Resolve(_RA_UserName, "_RA_UserName");
    Resolve(_RA_HardcoreModeIsActive, "_RA_HardcoreModeIsActive");
    Resolve(_RA_WarnDisableHardcore, "_RA_WarnDisableHardcore");
    Resolve(_RA_OnReset, "_RA_OnReset");
    Resolve(_RA_OnSaveState, "_RA_OnSaveState");
    Resolve(_RA_OnLoadState, "_RA_OnLoadState");
    Resolve(_RA_CaptureState, "_RA_CaptureState");
    Resolve(_RA_RestoreState, "_RA_RestoreState");

    /* No _RA_* entry point has run yet, so it can simply be unloaded. Its
       static constructors have already run, though, and dlclose will run
       its destructors. */
    const char* sMissing = (_RA_IntegrationVersion == nullptr) ? "_RA_IntegrationVersion"
                         : (_RA_Shutdown == nullptr)           ? "_RA_Shutdown"
                         : nullptr;
    if (sMissing != nullptr)
    {
        std::fprintf(stderr, "RA_Interface: %s does not export %s; achievements are disabled\n", g_sIntegrationPath.c_str(), sMissing);
        UnloadIntegration();
        return false;
    }

    return true;
}

static void RA_InitCommon(RA_WindowHandle hMainHWND, int nEmulatorID, const char* sClientName, const char* sClientVersion)
{
    if (!InstallIntegration())
        return;

    /* Which init entry point is needed depends on the caller, so it is checked
       here. RA_Shutdown is safe even if nothing has been initialised: the
       library's shutdown returns at once when it has nothing to shut down. */
    const bool bHasInit = (sClientName == nullptr) ? (_RA_InitI != nullptr) : (_RA_InitClient != nullptr);
    if (!bHasInit)
    {
        std::fprintf(stderr, "RA_Interface: %s does not export %s; achievements are disabled\n",
                     g_sIntegrationPath.c_str(), (sClientName == nullptr) ? "_RA_InitI" : "_RA_InitClient");
        RA_Shutdown();
        return;
    }

    /* The library reads its server from host.txt, and "OFFLINE" there selects
       the offline entry points, as in RA_Interface.cpp. On Linux the library
       registers the services this needs on first use, since there is no
       DllMain to do it at load time. */
    std::string sHostUrl;
    if (_RA_HostUrl != nullptr)
        sHostUrl = _RA_HostUrl();
    else if (_RA_HostName != nullptr)
        sHostUrl = std::string("http://") + _RA_HostName();

    if (sHostUrl == "http://OFFLINE")
    {
        /* as in RA_Interface.cpp, the offline entry points' result is not checked */
        if (sClientName == nullptr && _RA_InitOffline != nullptr)
        {
            _RA_InitOffline(hMainHWND, nEmulatorID, sClientVersion);
            return;
        }

        if (sClientName != nullptr && _RA_InitClientOffline != nullptr)
        {
            _RA_InitClientOffline(hMainHWND, sClientName, sClientVersion);
            return;
        }
    }

    const int nResult = (sClientName == nullptr) ? _RA_InitI(hMainHWND, nEmulatorID, sClientVersion)
                                                 : _RA_InitClient(hMainHWND, sClientName, sClientVersion);
    if (!nResult)
        RA_Shutdown();
}

void RA_Init(RA_WindowHandle hMainHWND, int nEmulatorID, const char* sClientVersion)
{
    RA_InitCommon(hMainHWND, nEmulatorID, nullptr, sClientVersion);
}

void RA_InitClient(RA_WindowHandle hMainHWND, const char* sClientName, const char* sClientVersion)
{
    RA_InitCommon(hMainHWND, -1, sClientName, sClientVersion);
}

void RA_SetUserAgentDetail(const char* sDetail)
{
    if (_RA_SetUserAgentDetail != nullptr)
        _RA_SetUserAgentDetail(sDetail);
}

void RA_InstallSharedFunctions(int(*)(void), void(*fpCauseUnpause)(void), void(*fpCausePause)(void), void(*fpRebuildMenu)(void), void(*fpEstimateTitle)(char*), void(*fpResetEmulation)(void), void(*fpLoadROM)(const char*))
{
    if (_RA_InstallSharedFunctions != nullptr)
        _RA_InstallSharedFunctions(nullptr, fpCauseUnpause, fpCausePause, fpRebuildMenu, fpEstimateTitle, fpResetEmulation, fpLoadROM);
}

void RA_Shutdown(void)
{
    //	Call shutdown on toolchain
    if (_RA_Shutdown != nullptr)
    {
        try {
            _RA_Shutdown();
        }
        catch (std::runtime_error&) {
        }
    }

    /* Once _RA_Shutdown has returned, no code of the library is running, so
       unloading it is safe whether or not dlclose unmaps it. */
    UnloadIntegration();
}
