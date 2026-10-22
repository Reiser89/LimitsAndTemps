#include "cputemp.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// PawnIO device interface (as used by PawnIOLib); each handle holds one loaded module.
const DWORD kPawnIoDeviceType = 41394u << 16;
const DWORD kIoctlLoadBinary = kPawnIoDeviceType | (0x821 << 2);
const DWORD kIoctlExecute = kPawnIoDeviceType | (0x841 << 2);
const size_t kFunctionNameBytes = 32;

const unsigned IA32_THERM_STATUS = 0x19C;
const unsigned IA32_TEMPERATURE_TARGET = 0x1A2;
const unsigned IA32_PACKAGE_THERM_STATUS = 0x1B1;
const unsigned long long kThermReadingValid = 0x80000000;

const unsigned F17H_THM_TCON_CUR_TMP = 0x00059800;
const unsigned F17H_TEMP_RANGE_SEL = 0x80000; // reading is 49 °C too high
const unsigned K10_REPORTED_TEMPERATURE_CONTROL = 0xA4;
const unsigned F15H_M60H_SMU_REPORTED_TEMP = 0xD8200CA4;

std::string BrandString()
{
    int regs[4];
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004)
        return std::string();
    char brand[49] = {};
    for (int i = 0; i < 3; ++i)
    {
        __cpuid(regs, 0x80000002 + i);
        memcpy(brand + i * 16, regs, 16);
    }
    return brand;
}

// "Intel(R) Xeon(R) CPU X5687 @ 3.60GHz" -> "Intel Xeon X5687",
// "AMD FX(tm)-8350 Eight-Core Processor" -> "AMD FX-8350".
std::wstring CleanName(std::string s)
{
    const char* noise[] = { "(R)", "(TM)", "(tm)", "CPU" };
    for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); ++i)
        for (size_t pos; (pos = s.find(noise[i])) != std::string::npos;)
            s.erase(pos, strlen(noise[i]));
    size_t coreCount = s.find("-Core Processor");
    if (coreCount != std::string::npos)
    {
        size_t start = s.rfind(' ', coreCount);
        start = start == std::string::npos ? 0 : start;
        s.erase(start, coreCount + strlen("-Core Processor") - start);
    }
    size_t at = s.find('@');
    if (at != std::string::npos)
        s.erase(at);

    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != ' ' || (!out.empty() && out.back() != L' '))
            out += static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
    while (!out.empty() && out.back() == L' ')
        out.pop_back();
    return out.empty() ? L"CPU" : out;
}

bool LoadModule(HANDLE device, const wchar_t* resource)
{
    HRSRC res = FindResourceW(NULL, resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(NULL, res) : NULL;
    const void* data = h ? LockResource(h) : NULL;
    DWORD got = 0;
    return data && DeviceIoControl(device, kIoctlLoadBinary, const_cast<void*>(data), SizeofResource(NULL, res),
                                   NULL, 0, &got, NULL);
}
} // namespace

CpuTemps::CpuTemps()
    : m_device(INVALID_HANDLE_VALUE), m_pciMutex(NULL), m_kind(CPU_NONE),
      m_family(0), m_model(0), m_stepping(0), m_packageSensor(false)
{
}

CpuTemps::~CpuTemps()
{
    if (m_device != INVALID_HANDLE_VALUE)
        CloseHandle(m_device);
    if (m_pciMutex)
        CloseHandle(m_pciMutex);
}

bool CpuTemps::Open(std::wstring& error)
{
    int regs[4];
    __cpuid(regs, 0);
    const int maxLeaf = regs[0];
    char vendor[13] = {};
    memcpy(vendor, &regs[1], 4);
    memcpy(vendor + 4, &regs[3], 4);
    memcpy(vendor + 8, &regs[2], 4);

    __cpuid(regs, 1);
    const int baseFamily = (regs[0] >> 8) & 0xF;
    const int baseModel = (regs[0] >> 4) & 0xF;
    m_family = baseFamily == 0xF ? baseFamily + ((regs[0] >> 20) & 0xFF) : baseFamily;
    m_model = baseFamily == 0x6 || baseFamily == 0xF ? baseModel | (((regs[0] >> 16) & 0xF) << 4) : baseModel;
    m_stepping = regs[0] & 0xF;
    m_brand = BrandString();

    const wchar_t* module = NULL;
    if (strcmp(vendor, "GenuineIntel") == 0 && maxLeaf >= 6)
    {
        __cpuid(regs, 6);
        if (regs[0] & 1) // digital thermal sensor
        {
            m_kind = CPU_INTEL;
            m_packageSensor = (regs[0] & 0x40) != 0;
            module = L"PAWNIO_INTELMSR";
        }
    }
    else if (strcmp(vendor, "AuthenticAMD") == 0)
    {
        if (m_family >= 0x17 && m_family <= 0x1A)
        {
            m_kind = CPU_AMD_ZEN;
            module = L"PAWNIO_AMDFAMILY17";
        }
        else if (m_family >= 0x10 && m_family <= 0x16)
        {
            // Only temperature registers are read: the module's clock measurement can freeze FX CPUs.
            m_kind = CPU_AMD_K10;
            module = L"PAWNIO_AMDFAMILY10";
        }
    }
    if (!module)
    {
        error = L"This CPU has no supported temperature sensor";
        return false;
    }

    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &bytes);
    std::vector<BYTE> buf(bytes);
    if (bytes == 0 || !GetLogicalProcessorInformationEx(
                          RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(&buf[0]), &bytes))
    {
        error = L"Could not read the CPU topology";
        return false;
    }
    std::vector<const PROCESSOR_RELATIONSHIP*> packages, cores;
    for (DWORD off = 0; off < bytes;)
    {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* info =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(&buf[off]);
        if (info->Relationship == RelationProcessorPackage)
            packages.push_back(&info->Processor);
        else if (info->Relationship == RelationProcessorCore)
            cores.push_back(&info->Processor);
        off += info->Size;
    }
    for (size_t i = 0; i < packages.size(); ++i)
    {
        Package p;
        p.tjMax = 100;
        p.last = kNaN;
        for (size_t c = 0; c < cores.size(); ++c)
        {
            const GROUP_AFFINITY& core = cores[c]->GroupMask[0];
            for (WORD g = 0; g < packages[i]->GroupCount; ++g)
                if (packages[i]->GroupMask[g].Group == core.Group && (packages[i]->GroupMask[g].Mask & core.Mask))
                {
                    Thread t = { core.Group, core.Mask & (~core.Mask + 1) }; // first thread of the core
                    p.cores.push_back(t);
                    break;
                }
        }
        if (!p.cores.empty())
            m_packages.push_back(p);
    }
    if (m_packages.empty())
    {
        error = L"Could not read the CPU topology";
        return false;
    }
    // The SMN registers are only reachable through the first socket.
    if (m_kind == CPU_AMD_ZEN)
        m_packages.resize(1);

    m_device = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (m_device == INVALID_HANDLE_VALUE)
    {
        error = GetLastError() == ERROR_ACCESS_DENIED ? L"Needs administrator rights" : L"PawnIO driver not installed";
        return false;
    }
    if (!LoadModule(m_device, module))
    {
        error = L"Could not load the PawnIO module";
        return false;
    }
    if (m_kind != CPU_INTEL)
        m_pciMutex = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");

    std::wstring name = CleanName(m_brand);
    for (size_t i = 0; i < m_packages.size(); ++i)
    {
        m_packages[i].name = m_packages.size() > 1 ? name + L" #" + std::to_wstring(i + 1) : name;
        if (m_kind == CPU_INTEL)
            m_packages[i].tjMax = IntelTjMax(m_packages[i]);
    }
    return true;
}

void CpuTemps::Read(std::vector<TempReading>& out)
{
    for (size_t i = 0; i < m_packages.size(); ++i)
    {
        Package& p = m_packages[i];
        double t = kNaN;
        switch (m_kind)
        {
        case CPU_INTEL:   t = ReadIntel(p); break;
        case CPU_AMD_ZEN: t = ReadAmdZen(p); break;
        case CPU_AMD_K10: t = ReadAmdK10(p, i); break;
        default:          break;
        }
        p.last = t;
        TempReading r = { SENSOR_CPU, p.name, t };
        out.push_back(r);
    }
}

std::wstring CpuTemps::Describe() const
{
    wchar_t buf[64];
    swprintf(buf, 64, L"family 0x%X model 0x%X stepping 0x%X", m_family, m_model, m_stepping);
    std::wstring brand(m_brand.begin(), m_brand.end());
    return brand + L" (" + buf + L"), " + std::to_wstring(m_packages.size()) + L" package(s)";
}

bool CpuTemps::Execute(const char* function, const unsigned long long* in, size_t inCount, unsigned long long& out)
{
    // Input: the function name in a fixed 32-byte field, followed by the 64-bit arguments.
    unsigned long long buf[kFunctionNameBytes / 8 + 2] = {};
    if (inCount > 2)
        return false;
    memcpy(buf, function, std::min(strlen(function), kFunctionNameBytes - 1));
    memcpy(buf + kFunctionNameBytes / 8, in, inCount * sizeof(*in));
    DWORD got = 0;
    return DeviceIoControl(m_device, kIoctlExecute, buf, static_cast<DWORD>(kFunctionNameBytes + inCount * sizeof(*in)),
                           &out, sizeof(out), &got, NULL) && got == sizeof(out);
}

bool CpuTemps::ReadMsr(const Thread* on, unsigned msr, unsigned long long& value)
{
    // MSRs are read on whichever CPU the calling thread runs on.
    GROUP_AFFINITY previous;
    if (on)
    {
        GROUP_AFFINITY target = {};
        target.Group = on->group;
        target.Mask = static_cast<KAFFINITY>(on->mask);
        if (!SetThreadGroupAffinity(GetCurrentThread(), &target, &previous))
            return false;
    }
    unsigned long long in = msr;
    bool ok = Execute("ioctl_read_msr", &in, 1, value);
    if (on)
        SetThreadGroupAffinity(GetCurrentThread(), &previous, NULL);
    return ok;
}

bool CpuTemps::LockPciBus()
{
    if (!m_pciMutex)
        return true;
    DWORD w = WaitForSingleObject(m_pciMutex, 10);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

void CpuTemps::UnlockPciBus()
{
    if (m_pciMutex)
        ReleaseMutex(m_pciMutex);
}

double CpuTemps::IntelTjMax(const Package& p)
{
    if (m_family == 0x6)
    {
        // Pre-Nehalem parts have no IA32_TEMPERATURE_TARGET; values from their datasheets.
        switch (m_model)
        {
        case 0x0F: // Core 2 (65 nm)
            if (m_stepping == 0x06)
                return p.cores.size() == 2 ? 90 : p.cores.size() == 4 ? 100 : 95;
            return m_stepping == 0x0B ? 100 : 95;
        case 0x17: // Core 2 (45 nm)
            return 100;
        case 0x1C: // Atom (45 nm)
            return m_stepping == 0x0A ? 100 : 90;
        }
    }
    unsigned long long v;
    if (ReadMsr(&p.cores[0], IA32_TEMPERATURE_TARGET, v) && ((v >> 16) & 0xFF) != 0)
        return static_cast<double>((v >> 16) & 0xFF);
    return 100;
}

double CpuTemps::ReadIntel(const Package& p)
{
    unsigned long long v;
    // The package sensor is package-scoped, so with one socket any CPU will do and the thread stays where it is.
    const Thread* packageThread = m_packages.size() > 1 ? &p.cores[0] : NULL;
    if (m_packageSensor && ReadMsr(packageThread, IA32_PACKAGE_THERM_STATUS, v) && (v & kThermReadingValid))
        return p.tjMax - static_cast<double>((v >> 16) & 0x7F);

    // No package sensor (e.g. Nehalem/Westmere): the hottest core.
    double hottest = kNaN;
    for (size_t i = 0; i < p.cores.size(); ++i)
        if (ReadMsr(&p.cores[i], IA32_THERM_STATUS, v) && (v & kThermReadingValid))
        {
            double t = p.tjMax - static_cast<double>((v >> 16) & 0x7F);
            if (std::isnan(hottest) || t > hottest)
                hottest = t;
        }
    return hottest;
}

double CpuTemps::ReadAmdZen(Package& p)
{
    if (!LockPciBus())
        return p.last;
    unsigned long long in = F17H_THM_TCON_CUR_TMP, v = 0;
    bool ok = Execute("ioctl_read_smn", &in, 1, v);
    UnlockPciBus();
    if (!ok)
        return kNaN;

    double t = ((v >> 21) & 0x7FF) * 0.125;
    if (v & F17H_TEMP_RANGE_SEL)
        t -= 49;
    // Tctl runs ahead of the real die temperature on some early Zen parts (offsets from Linux k10temp).
    const std::string& b = m_brand;
    if (b.find("1600X") != std::string::npos || b.find("1700X") != std::string::npos || b.find("1800X") != std::string::npos)
        t -= 20;
    else if (b.find("Threadripper 19") != std::string::npos || b.find("Threadripper 29") != std::string::npos)
        t -= 27;
    else if (b.find("2700X") != std::string::npos)
        t -= 10;
    return t;
}

double CpuTemps::ReadAmdK10(Package& p, size_t node)
{
    unsigned long long v = 0;
    bool ok;
    if (m_family == 0x15 && ((m_model & 0xF0) == 0x60 || (m_model & 0xF0) == 0x70))
    {
        if (!LockPciBus())
            return p.last;
        unsigned long long in = F15H_M60H_SMU_REPORTED_TEMP;
        ok = Execute("ioctl_read_smu", &in, 1, v);
        UnlockPciBus();
    }
    else
    {
        unsigned long long in[2] = { node, K10_REPORTED_TEMPERATURE_CONTROL };
        ok = Execute("ioctl_read_miscctl", in, 2, v);
    }
    if (!ok)
        return kNaN;
    const unsigned mask = m_family == 0x15 && (m_model & 0xF0) == 0x00 ? 0x7FC : 0x7FF;
    return ((v >> 21) & mask) / 8.0;
}
