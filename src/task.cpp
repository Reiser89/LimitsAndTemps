#include "task.h"

#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <taskschd.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace
{
const wchar_t kTaskPrefix[] = L"LimitsAndTemps CPU helper ";

struct ComInit
{
    ComInit() : hr(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)) {}
    ~ComInit()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
    HRESULT hr;
};

// Releases a COM pointer at scope end.
template <class T> struct ComPtr
{
    ComPtr() : p(NULL) {}
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    T* p;
};

struct Bstr
{
    explicit Bstr(const std::wstring& s) : b(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(b); }
    BSTR b;
};

std::wstring UserSid()
{
    std::wstring sid;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return sid;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    std::vector<BYTE> buf(size ? size : 1);
    LPWSTR text = NULL;
    if (size && GetTokenInformation(token, TokenUser, &buf[0], size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&buf[0])->User.Sid, &text))
    {
        sid = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return sid;
}

// One task per user, so several accounts on one PC each get their own.
std::wstring TaskName()
{
    return kTaskPrefix + UserSid();
}

std::wstring SelfPath()
{
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH * 4);
    return n == 0 || n >= MAX_PATH * 4 ? std::wstring() : std::wstring(path, n);
}

// Program Files is writable by administrators only.
std::wstring InstallDir()
{
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, NULL, &base)))
        return std::wstring();
    std::wstring dir = std::wstring(base) + L"\\LimitsAndTemps";
    CoTaskMemFree(base);
    return dir;
}

bool FilesEqual(const std::wstring& a, const std::wstring& b)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE fa = CreateFileW(a.c_str(), GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE fb = CreateFileW(b.c_str(), GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    bool equal = false;
    LARGE_INTEGER sa, sb;
    if (fa != INVALID_HANDLE_VALUE && fb != INVALID_HANDLE_VALUE && GetFileSizeEx(fa, &sa) && GetFileSizeEx(fb, &sb) &&
        sa.QuadPart == sb.QuadPart)
    {
        std::vector<char> ba(64 * 1024), bb(64 * 1024);
        for (;;)
        {
            DWORD ga = 0, gb = 0;
            if (!ReadFile(fa, &ba[0], static_cast<DWORD>(ba.size()), &ga, NULL) ||
                !ReadFile(fb, &bb[0], static_cast<DWORD>(bb.size()), &gb, NULL) || ga != gb ||
                memcmp(&ba[0], &bb[0], ga) != 0)
                break;
            if (ga == 0)
            {
                equal = true;
                break;
            }
        }
    }
    if (fa != INVALID_HANDLE_VALUE)
        CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE)
        CloseHandle(fb);
    return equal;
}

bool RootFolder(ComPtr<ITaskFolder>& folder)
{
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                reinterpret_cast<void**>(&service.p))))
        return false;
    VARIANT empty;
    VariantInit(&empty);
    Bstr root(L"\\");
    return SUCCEEDED(service.p->Connect(empty, empty, empty, empty)) &&
           SUCCEEDED(service.p->GetFolder(root.b, &folder.p));
}

std::wstring XmlEscape(const std::wstring& s)
{
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i)
        switch (s[i])
        {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        default:   out += s[i]; break;
        }
    return out;
}
} // namespace

bool HelperTask::IsUsable()
{
    const std::wstring dir = InstallDir();
    if (dir.empty() || !FilesEqual(SelfPath(), dir + L"\\LimitsAndTemps.exe"))
        return false;
    ComInit com;
    ComPtr<ITaskFolder> folder;
    ComPtr<IRegisteredTask> task;
    Bstr name(TaskName());
    return RootFolder(folder) && SUCCEEDED(folder.p->GetTask(name.b, &task.p));
}

bool HelperTask::Run(const std::wstring& parentPid, const std::wstring& mapName)
{
    ComInit com;
    ComPtr<ITaskFolder> folder;
    ComPtr<IRegisteredTask> task;
    Bstr name(TaskName());
    if (!RootFolder(folder) || FAILED(folder.p->GetTask(name.b, &task.p)))
        return false;

    // Become $(Arg0) and $(Arg1) in the task's action. Run only takes a BSTR array, not a VARIANT array.
    SAFEARRAY* args = SafeArrayCreateVector(VT_BSTR, 0, 2);
    if (!args)
        return false;
    const std::wstring values[2] = { parentPid, mapName };
    for (LONG i = 0; i < 2; ++i)
    {
        Bstr value(values[i]);
        SafeArrayPutElement(args, &i, value.b); // copies the string
    }
    VARIANT params;
    VariantInit(&params);
    params.vt = VT_ARRAY | VT_BSTR;
    params.parray = args;
    ComPtr<IRunningTask> running;
    HRESULT hr = task.p->Run(params, &running.p);
    SafeArrayDestroy(args);
    return SUCCEEDED(hr);
}

bool HelperTask::Register(std::wstring& error)
{
    const std::wstring self = SelfPath();
    const std::wstring dir = InstallDir();
    const std::wstring sid = UserSid();
    if (self.empty() || dir.empty() || sid.empty())
    {
        error = L"Could not set up start without prompt";
        return false;
    }
    const std::wstring exe = dir + L"\\LimitsAndTemps.exe";
    CreateDirectoryW(dir.c_str(), NULL);
    if (!FilesEqual(self, exe) && !CopyFileW(self.c_str(), exe.c_str(), FALSE))
    {
        error = L"Could not copy LimitsAndTemps.exe to " + dir;
        return false;
    }

    // On demand only (no triggers); runs as this user in their session, with their full admin token.
    // The helper validates its arguments, since anyone running as this user can start the task.
    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Starts the LimitsAndTemps CPU temperature helper with administrator rights, "
        L"without a UAC prompt.</Description></RegistrationInfo>"
        L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>true</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
        L"<Arguments>--helper --pid $(Arg0) --map $(Arg1)</Arguments></Exec></Actions></Task>";

    ComInit com;
    ComPtr<ITaskFolder> folder;
    ComPtr<IRegisteredTask> task;
    Bstr name(TaskName()), definition(xml);
    VARIANT empty, sddl;
    VariantInit(&empty);
    VariantInit(&sddl);
    sddl.vt = VT_BSTR;
    sddl.bstrVal = SysAllocString((L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;" + sid + L")").c_str());
    HRESULT hr = E_FAIL;
    if (RootFolder(folder))
        hr = folder.p->RegisterTask(name.b, definition.b, TASK_CREATE_OR_UPDATE, empty, empty,
                                    TASK_LOGON_INTERACTIVE_TOKEN, sddl, &task.p);
    VariantClear(&sddl);
    if (FAILED(hr))
    {
        wchar_t code[16];
        swprintf(code, 16, L"0x%08X", static_cast<unsigned>(hr));
        error = std::wstring(L"Could not register the start task (") + code + L")";
        return false;
    }
    return true;
}

void HelperTask::Unregister()
{
    ComInit com;
    ComPtr<ITaskFolder> folder;
    if (!RootFolder(folder))
        return;
    Bstr name(TaskName());
    folder.p->DeleteTask(name.b, 0);

    // The copy is shared by all users' tasks.
    bool othersNeedCopy = false;
    ComPtr<IRegisteredTaskCollection> tasks;
    LONG count = 0;
    if (SUCCEEDED(folder.p->GetTasks(TASK_ENUM_HIDDEN, &tasks.p)) && SUCCEEDED(tasks.p->get_Count(&count)))
        for (LONG i = 1; i <= count && !othersNeedCopy; ++i)
        {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            ComPtr<IRegisteredTask> t;
            BSTR taskName = NULL;
            if (SUCCEEDED(tasks.p->get_Item(index, &t.p)) && SUCCEEDED(t.p->get_Name(&taskName)))
            {
                othersNeedCopy = wcsncmp(taskName, kTaskPrefix, wcslen(kTaskPrefix)) == 0;
                SysFreeString(taskName);
            }
        }
    if (othersNeedCopy)
        return;

    // The helper that ran from the copy may still be exiting.
    const std::wstring dir = InstallDir();
    const std::wstring exe = dir + L"\\LimitsAndTemps.exe";
    for (int i = 0; i < 25 && !DeleteFileW(exe.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; ++i)
        Sleep(200);
    RemoveDirectoryW(dir.c_str());
}
