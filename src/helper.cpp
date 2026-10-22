#include "helper.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "cputemp.h"
#include "task.h"
#include "usage.h"
#include "version.h"

namespace
{
// Must match docs/ipc.md.
const DWORD kMapBytes = 16384;
const DWORD kHeaderBytes = 16; // seq, byte length, quit flag, helper PID
const wchar_t kChannelPrefix[] = L"Local\\LimitsAndTemps-Helper-";
const DWORD kStartupTimeoutMs = 5000;

bool IsElevated()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;)
    {
        size_t pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start));
        if (pos == std::wstring::npos)
            return parts;
        start = pos + 1;
    }
}

std::wstring Format(const std::vector<TempReading>& readings, const std::wstring& status)
{
    std::wstring text;
    if (!status.empty())
        text += L"S\t" + status + L"\n";
    for (size_t i = 0; i < readings.size(); ++i)
    {
        std::wstring name = readings[i].name;
        std::replace(name.begin(), name.end(), L'\t', L' ');
        text += L"T\t" + name + L"\t";
        if (!std::isnan(readings[i].celsius))
            text += std::to_wstring(std::lround(readings[i].celsius * 10)); // tenths: no locale-dependent decimals
        text += L"\n";
    }
    return text;
}

void Publish(void* view, LONG& seq, const std::wstring& text)
{
    volatile LONG* header = static_cast<volatile LONG*>(view);
    DWORD len = static_cast<DWORD>(std::min<size_t>(text.size() * sizeof(wchar_t), kMapBytes - kHeaderBytes)) & ~1u;
    header[0] = ++seq; // odd: write in progress
    MemoryBarrier();
    header[1] = static_cast<LONG>(len);
    memcpy(static_cast<char*>(view) + kHeaderBytes, text.data(), len);
    MemoryBarrier();
    header[0] = ++seq; // even: consistent
}

// Runs the embedded official PawnIO installer silently. Runs elevated, from a fresh directory
// only administrators can write to, so an unelevated process can't swap the file before it runs.
bool InstallPawnIo(std::wstring& error)
{
    HRSRC res = FindResourceW(NULL, L"PAWNIOSETUP", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(NULL, res) : NULL;
    const void* data = h ? LockResource(h) : NULL;
    const DWORD size = res ? SizeofResource(NULL, res) : 0;
    wchar_t windir[MAX_PATH] = {};
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!data || !GetWindowsDirectoryW(windir, MAX_PATH) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", SDDL_REVISION_1,
                                                              &sd, NULL))
    {
        error = L"PawnIO installer missing";
        return false;
    }
    LARGE_INTEGER unique;
    QueryPerformanceCounter(&unique);
    const std::wstring dir = std::wstring(windir) + L"\\Temp\\LimitsAndTemps-" +
                             std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(unique.QuadPart);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    BOOL created = CreateDirectoryW(dir.c_str(), &sa);
    LocalFree(sd);
    if (!created)
    {
        error = L"Could not prepare the PawnIO installer";
        return false;
    }

    const std::wstring setup = dir + L"\\PawnIO_setup.exe";
    HANDLE file = CreateFileW(setup.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, data, size, &written, NULL) && written == size;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);

    DWORD exitCode = 1;
    if (ok)
    {
        std::wstring cmd = L"\"" + setup + L"\" -install -silent";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        ok = CreateProcessW(setup.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi) != FALSE;
        if (ok)
        {
            ok = WaitForSingleObject(pi.hProcess, 5 * 60 * 1000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    DeleteFileW(setup.c_str());
    RemoveDirectoryW(dir.c_str());

    if (!HelperClient::IsPawnIoInstalled())
    {
        error = L"PawnIO installation failed (code " + std::to_wstring(exitCode) + L")";
        return false;
    }
    return true;
}
} // namespace

HelperClient::HelperClient()
    : m_map(NULL), m_view(NULL), m_request(NULL), m_done(NULL), m_process(NULL), m_lastSeq(0)
{
}

HelperClient::~HelperClient()
{
    Stop();
}

bool HelperClient::CreateChannel()
{
    // Fresh names per start, so a previous helper that is still shutting down can't write into them.
    // Objects that already exist were planted by someone else: refuse them.
    static int generation = 0;
    m_name = kChannelPrefix + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++generation);
    m_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, kMapBytes, m_name.c_str());
    if (!m_map || GetLastError() == ERROR_ALREADY_EXISTS)
        return false;
    m_request = CreateEventW(NULL, FALSE, FALSE, (m_name + L"-request").c_str());
    if (!m_request || GetLastError() == ERROR_ALREADY_EXISTS)
        return false;
    m_done = CreateEventW(NULL, FALSE, FALSE, (m_name + L"-done").c_str());
    if (!m_done || GetLastError() == ERROR_ALREADY_EXISTS)
        return false;
    m_view = MapViewOfFile(m_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMapBytes);
    return m_view != NULL;
}

bool HelperClient::Start(bool installPawnIo, std::wstring& error)
{
    Stop();
    if (!CreateChannel())
    {
        Stop();
        error = L"Could not create shared memory";
        return false;
    }
    m_lastSeq = 0;
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const bool elevated = IsElevated();

    // After the first consent, a scheduled task starts the helper elevated without a UAC prompt.
    // The helper signals -done once it is attached and has published its PID.
    if (!installPawnIo && !elevated && HelperTask::IsUsable())
    {
        if (HelperTask::Run(pid, m_name) && WaitForSingleObject(m_done, kStartupTimeoutMs) == WAIT_OBJECT_0)
        {
            const DWORD helperPid = static_cast<DWORD>(static_cast<volatile LONG*>(m_view)[3]);
            m_process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, helperPid);
            if (m_process)
                return true;
        }
        // Fresh names, so a helper that starts late can't attach to the UAC-launched one's channel.
        Stop();
        if (!CreateChannel())
        {
            Stop();
            error = L"Could not create shared memory";
            return false;
        }
    }

    wchar_t self[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4)
    {
        Stop();
        error = L"Could not locate LimitsAndTemps.exe";
        return false;
    }
    // Deny writers and renames while UAC elevates the file and the helper copies it for the start task,
    // so what runs elevated, now and later, is this very exe.
    HANDLE lock = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    std::wstring args = L"--helper --pid " + pid + L" --map " + m_name + L" --register-task";
    if (installPawnIo)
        args += L" --install-pawnio";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = elevated ? L"open" : L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    BOOL ok = ShellExecuteExW(&sei);
    DWORD err = GetLastError();
    if (ok && sei.hProcess)
    {
        m_process = sei.hProcess;
        HANDLE waits[2] = { m_done, m_process };
        WaitForMultipleObjects(2, waits, FALSE, kStartupTimeoutMs);
    }
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);

    if (!m_process)
    {
        Stop();
        error = err == ERROR_CANCELLED ? L"Administrator permission was declined"
                                       : L"Could not start helper (error " + std::to_wstring(err) + L")";
        return false;
    }
    return true;
}

void HelperClient::Stop()
{
    // An elevated helper can't be terminated from here; ask it to quit via the shared flag.
    // It also exits by itself when this process ends.
    if (m_view)
    {
        static_cast<volatile LONG*>(m_view)[2] = 1;
        if (m_request)
            SetEvent(m_request);
        UnmapViewOfFile(m_view);
        m_view = NULL;
    }
    HANDLE* handles[] = { &m_map, &m_request, &m_done, &m_process };
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i)
        if (*handles[i])
        {
            CloseHandle(*handles[i]);
            *handles[i] = NULL;
        }
    m_last.clear();
    m_status.clear();
}

bool HelperClient::IsPawnIoInstalled()
{
    wchar_t image[MAX_PATH * 2] = {};
    DWORD size = sizeof(image) - sizeof(wchar_t);
    // Read the service key directly: it is readable unelevated, unlike the driver itself.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO", L"ImagePath",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, image, &size) != ERROR_SUCCESS)
        return false;

    // ImagePath is "\SystemRoot\..." or an absolute path; map it to a Win32 path.
    std::wstring path = image;
    const std::wstring root = L"\\SystemRoot\\";
    if (path.compare(0, root.size(), root) == 0)
    {
        wchar_t windir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(windir, MAX_PATH))
            return false;
        path = std::wstring(windir) + L"\\" + path.substr(root.size());
    }
    else if (path.compare(0, 4, L"\\??\\") == 0)
        path.erase(0, 4);

    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool HelperClient::IsRunning() const
{
    return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

void HelperClient::Read(std::vector<TempReading>& out, std::wstring& status)
{
    out.clear();
    status.clear();
    if (!m_view)
        return;

    if (!IsRunning())
    {
        m_status = L"Helper stopped";
        m_last.clear();
        status = m_status;
        return;
    }

    // The helper answers within a millisecond; the timeout only guards against a stuck helper,
    // in which case the previous readings are kept.
    ResetEvent(m_done);
    SetEvent(m_request);
    WaitForSingleObject(m_done, 200);

    volatile LONG* header = static_cast<volatile LONG*>(m_view);
    LONG seq1 = header[0];
    if (seq1 != 0 && (seq1 & 1) == 0 && seq1 != m_lastSeq)
    {
        MemoryBarrier();
        LONG len = header[1];
        std::wstring text;
        if (len > 0 && static_cast<DWORD>(len) <= kMapBytes - kHeaderBytes)
            text.assign(reinterpret_cast<const wchar_t*>(static_cast<const char*>(m_view) + kHeaderBytes),
                        len / sizeof(wchar_t));
        MemoryBarrier();
        if (header[0] == seq1)
        {
            m_lastSeq = seq1;
            m_last.clear();
            m_status.clear();
            std::vector<std::wstring> lines = Split(text, L'\n');
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::vector<std::wstring> f = Split(lines[i], L'\t');
                if (f.size() >= 2 && f[0] == L"S")
                    m_status = f[1];
                else if (f.size() >= 3 && f[0] == L"T")
                {
                    TempReading r = { SENSOR_CPU, f[1], std::numeric_limits<double>::quiet_NaN() };
                    if (!f[2].empty())
                        r.celsius = wcstol(f[2].c_str(), NULL, 10) / 10.0;
                    m_last.push_back(r);
                }
            }
        }
    }

    out = m_last;
    status = m_status;
}

int RunHelper(int argc, wchar_t** argv)
{
    DWORD parentPid = 0;
    std::wstring name;
    bool installPawnIo = false, registerTask = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--pid" && i + 1 < argc)
            parentPid = wcstoul(argv[++i], NULL, 10);
        else if (a == L"--map" && i + 1 < argc)
            name = argv[++i];
        else if (a == L"--install-pawnio")
            installPawnIo = true;
        else if (a == L"--register-task")
            registerTask = true;
    }
    // Anything running as this user can start the task with arbitrary arguments,
    // so only ever attach to objects named like the app's own.
    if (parentPid == 0 || name.compare(0, wcslen(kChannelPrefix), kChannelPrefix) != 0)
        return 1;

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    void* view = map ? MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMapBytes) : NULL;
    HANDLE request = OpenEventW(SYNCHRONIZE, FALSE, (name + L"-request").c_str());
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-done").c_str());
    if (!parent || !view || !request || !done)
        return 1;

    // A failure here only means the next start asks UAC again.
    std::wstring ignored;
    if (registerTask)
        HelperTask::Register(ignored);
    volatile LONG* header = static_cast<volatile LONG*>(view);
    header[3] = static_cast<LONG>(GetCurrentProcessId());
    SetEvent(done);

    std::wstring status;
    if (installPawnIo && !HelperClient::IsPawnIoInstalled())
        InstallPawnIo(status);
    CpuTemps cpu;
    std::wstring error;
    const bool ok = cpu.Open(error);
    if (!ok && status.empty())
        status = error;

    // The app's UI thread waits for each answer.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Idle until asked: no timer of its own, so no work while nobody samples.
    LONG seq = 0;
    HANDLE waits[2] = { parent, request };
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 && header[2] == 0)
    {
        std::vector<TempReading> readings;
        if (ok)
            cpu.Read(readings);
        Publish(view, seq, Format(readings, status));
        SetEvent(done);
    }
    return 0;
}

int RunDump(const wchar_t* file)
{
    std::wstring text = std::wstring(L"LimitsAndTemps ") + APP_VERSION_W + L" sensor dump\n";
    CpuTemps cpu;
    std::wstring error;
    const bool ok = cpu.Open(error);
    text += L"CPU: " + cpu.Describe() + L"\n";
    text += std::wstring(L"PawnIO installed: ") + (HelperClient::IsPawnIoInstalled() ? L"yes" : L"no") + L"\n";
    text += std::wstring(L"Elevated: ") + (IsElevated() ? L"yes" : L"no") + L"\n";
    text += std::wstring(L"Start without prompt: ") + (HelperTask::IsUsable() ? L"set up" : L"not set up") + L"\n";
    if (!ok)
        text += L"CPU sensors: " + error + L"\n";

    std::vector<TempReading> readings;
    if (ok)
        cpu.Read(readings);
    Sensors sensors;
    std::vector<TempReading> native = sensors.Read(true);
    readings.insert(readings.end(), native.begin(), native.end());

    const wchar_t* kinds[] = { L"CPU", L"GPU", L"Drive", L"ACPI" };
    for (size_t i = 0; i < readings.size(); ++i)
    {
        wchar_t value[32] = L"n/a";
        if (!std::isnan(readings[i].celsius))
            swprintf(value, 32, L"%.1f C", readings[i].celsius);
        text += std::wstring(kinds[readings[i].kind]) + L"\t" + readings[i].name + L"\t" + value + L"\n";
    }

    std::vector<AgentUsage> usage = FetchUsage();
    for (size_t i = 0; i < usage.size(); ++i)
    {
        const UsageWindow* windows[] = { &usage[i].fiveHour, &usage[i].week, &usage[i].fableWeek };
        const wchar_t* labels[] = { L"5h", L"1w", L"Fable 1w" };
        text += L"Agent\t" + usage[i].name;
        for (int w = 0; w < 3; ++w)
        {
            wchar_t value[64];
            if (std::isnan(windows[w]->percent))
                continue;
            swprintf(value, 64, L"\t%s %.0f%% (reset %lld)", labels[w], windows[w]->percent, windows[w]->resetsAt);
            text += value;
        }
        if (!usage[i].error.empty())
            text += L"\t" + usage[i].error;
        text += L"\n";
    }

    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0, NULL, NULL);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], bytes, NULL, NULL);
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool wrote = f != INVALID_HANDLE_VALUE && WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, NULL);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    return wrote ? 0 : 1;
}
