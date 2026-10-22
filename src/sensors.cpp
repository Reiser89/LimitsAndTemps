#include "sensors.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cmath>
#include <limits>

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const unsigned long long kDiskIntervalMs = 30000; // drive temperatures move slowly, and each query is a command to the drive
const unsigned long long kGpuRescanDelayMs = 30000;
const UINT kVendorNvidia = 0x10DE;

// Minimal NVML subset; signatures from nvml.h.
typedef int (*NvmlInit)();
typedef int (*NvmlShutdown)();
typedef int (*NvmlGetCount)(unsigned int*);
typedef int (*NvmlGetHandle)(unsigned int, void**);
typedef int (*NvmlGetName)(void*, char*, unsigned int);
typedef int (*NvmlGetTemp)(void*, int, unsigned int*);
const int NVML_SUCCESS = 0;
const int NVML_TEMPERATURE_GPU = 0;

NvmlGetTemp g_nvmlGetTemp = NULL;

std::wstring Widen(const char* s)
{
    std::wstring out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n > 1)
    {
        out.resize(n - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    }
    return out;
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L' ');
    size_t e = s.find_last_not_of(L' ');
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

bool QueryAdapter(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type, void* data, UINT size)
{
    D3DKMT_QUERYADAPTERINFO q = {};
    q.hAdapter = adapter;
    q.Type = type;
    q.pPrivateDriverData = data;
    q.PrivateDriverDataSize = size;
    return D3DKMTQueryAdapterInfo(&q) == 0;
}

void CloseAdapter(D3DKMT_HANDLE adapter)
{
    D3DKMT_CLOSEADAPTER c = { adapter };
    D3DKMTCloseAdapter(&c);
}

bool QueryDiskTemp(HANDLE h, double& celsius)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;

    BYTE buf[512];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL))
        return false;

    const STORAGE_TEMPERATURE_DATA_DESCRIPTOR* d =
        reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf);
    if (got < sizeof(*d) || d->InfoCount == 0)
        return false;

    // Index 0 is the composite / overall device temperature.
    celsius = d->TemperatureInfo[0].Temperature;
    return true;
}

std::wstring QueryDiskName(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;

    BYTE buf[1024];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL))
        return std::wstring();

    const STORAGE_DEVICE_DESCRIPTOR* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
    if (d->ProductIdOffset == 0 || d->ProductIdOffset >= got)
        return std::wstring();
    return Trim(Widen(reinterpret_cast<const char*>(buf + d->ProductIdOffset)));
}
} // namespace

Sensors::Sensors()
    : m_nvml(NULL), m_gpuRescanAt(0), m_nextDiskRead(0), m_zonesTried(false), m_pdhQuery(NULL), m_pdhCounter(NULL)
{
    InitGpus();
    InitDisks();
}

Sensors::~Sensors()
{
    CloseGpus();
    for (size_t i = 0; i < m_disks.size(); ++i)
        CloseHandle(m_disks[i].handle);

    if (m_nvml)
    {
        NvmlShutdown shutdown = (NvmlShutdown)GetProcAddress((HMODULE)m_nvml, "nvmlShutdown");
        if (shutdown)
            shutdown();
        FreeLibrary((HMODULE)m_nvml);
    }

    if (m_pdhQuery)
        PdhCloseQuery(m_pdhQuery);
}

void Sensors::InitGpus()
{
    D3DKMT_ENUMADAPTERS2 e = {};
    if (D3DKMTEnumAdapters2(&e) != 0 || e.NumAdapters == 0)
        return;
    std::vector<D3DKMT_ADAPTERINFO> adapters(e.NumAdapters);
    e.pAdapters = &adapters[0];
    if (D3DKMTEnumAdapters2(&e) != 0)
        return;

    bool nvidiaNeedsNvml = false;
    std::vector<Gpu> found;
    std::vector<bool> foundNvidia;
    for (ULONG i = 0; i < e.NumAdapters; ++i)
    {
        const D3DKMT_HANDLE h = adapters[i].hAdapter;
        D3DKMT_ADAPTERTYPE type = {};
        if (!QueryAdapter(h, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type)) || type.SoftwareDevice || !type.RenderSupported)
        {
            CloseAdapter(h);
            continue;
        }
        D3DKMT_ADAPTERREGISTRYINFO reg = {};
        D3DKMT_QUERY_DEVICE_IDS ids = {};
        QueryAdapter(h, KMTQAITYPE_ADAPTERREGISTRYINFO, &reg, sizeof(reg));
        QueryAdapter(h, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids));
        Gpu gpu = { h, NULL, Trim(reg.AdapterString) };
        if (gpu.name.empty())
            gpu.name = L"GPU";
        const bool nvidia = ids.DeviceIds.VendorID == kVendorNvidia || gpu.name.find(L"NVIDIA") != std::wstring::npos;

        // Adapter perf data (with temperature) arrived with WDDM 2.4 drivers.
        D3DKMT_DRIVERVERSION version = KMT_DRIVERVERSION_WDDM_1_0;
        D3DKMT_ADAPTER_PERFDATA perf = {};
        if (QueryAdapter(h, KMTQAITYPE_DRIVERVERSION, &version, sizeof(version)) && version >= KMT_DRIVERVERSION_WDDM_2_4 &&
            QueryAdapter(h, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf)))
        {
            found.push_back(gpu);
            foundNvidia.push_back(nvidia);
        }
        else
        {
            nvidiaNeedsNvml = nvidiaNeedsNvml || nvidia;
            CloseAdapter(h);
        }
    }

    for (size_t i = 0; i < found.size(); ++i)
    {
        // One driver serves all NVIDIA GPUs: if it is too old for perf data, NVML reads them all.
        if (nvidiaNeedsNvml && foundNvidia[i])
            CloseAdapter(found[i].adapter);
        else
            m_gpus.push_back(found[i]);
    }
    if (nvidiaNeedsNvml)
        InitNvml();

    // Identical cards need distinct names: rows and their history are keyed by name.
    for (size_t i = 0; i < m_gpus.size(); ++i)
    {
        int same = 0, index = 0;
        for (size_t j = 0; j < m_gpus.size(); ++j)
            if (m_gpus[j].name == m_gpus[i].name)
            {
                ++same;
                if (j < i)
                    ++index;
            }
        if (same > 1)
            m_gpus[i].name += L" #" + std::to_wstring(index + 1);
    }
}

void Sensors::CloseGpus()
{
    for (size_t i = 0; i < m_gpus.size(); ++i)
        if (!m_gpus[i].nvmlDevice)
            CloseAdapter(m_gpus[i].adapter);
    m_gpus.clear();
}

void Sensors::InitNvml()
{
    if (!m_nvml)
    {
        // Newer drivers put nvml.dll in System32, older ones in NVSMI.
        HMODULE lib = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib)
            lib = LoadLibraryExW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll", NULL,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib)
            return;
        NvmlInit init = (NvmlInit)GetProcAddress(lib, "nvmlInit_v2");
        g_nvmlGetTemp = (NvmlGetTemp)GetProcAddress(lib, "nvmlDeviceGetTemperature");
        if (!init || !g_nvmlGetTemp || init() != NVML_SUCCESS)
        {
            FreeLibrary(lib);
            return;
        }
        m_nvml = lib;
    }

    HMODULE lib = (HMODULE)m_nvml;
    NvmlGetCount count   = (NvmlGetCount)GetProcAddress(lib, "nvmlDeviceGetCount_v2");
    NvmlGetHandle handle = (NvmlGetHandle)GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
    NvmlGetName name     = (NvmlGetName)GetProcAddress(lib, "nvmlDeviceGetName");
    unsigned int n = 0;
    if (!count || !handle || !name || count(&n) != NVML_SUCCESS)
        return;
    for (unsigned int i = 0; i < n; ++i)
    {
        Gpu gpu = { 0, NULL, std::wstring() };
        if (handle(i, &gpu.nvmlDevice) != NVML_SUCCESS)
            continue;
        char buf[96] = {};
        gpu.name = name(gpu.nvmlDevice, buf, sizeof(buf)) == NVML_SUCCESS ? Widen(buf) : L"NVIDIA GPU";
        m_gpus.push_back(gpu);
    }
}

void Sensors::InitDisks()
{
    for (int i = 0; i < 16; ++i)
    {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);

        // Zero access rights: enough for property queries, no admin required.
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        // Keep a spun-down drive without asking: the query could wake it.
        BOOL on = TRUE;
        double t;
        const bool asleep = GetDevicePowerState(h, &on) && !on;
        if (!asleep && !QueryDiskTemp(h, t))
        {
            CloseHandle(h);
            continue;
        }

        Disk disk = { h, QueryDiskName(h), kNaN };
        if (disk.name.empty())
            disk.name = L"Disk " + std::to_wstring(i);
        m_disks.push_back(disk);
    }
}

void Sensors::InitThermalZones()
{
    PDH_HQUERY query;
    if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS)
        return;

    PDH_HCOUNTER counter;
    if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0, &counter) != ERROR_SUCCESS ||
        PdhCollectQueryData(query) != ERROR_SUCCESS)
    {
        PdhCloseQuery(query);
        return;
    }

    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, NULL) != PDH_MORE_DATA)
    {
        PdhCloseQuery(query);
        return;
    }
    std::vector<BYTE> buf(bytes);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(&buf[0]);
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS || count == 0)
    {
        PdhCloseQuery(query);
        return;
    }

    for (DWORD i = 0; i < count; ++i)
        m_zones.push_back(items[i].szName);
    m_pdhQuery = query;
    m_pdhCounter = counter;
}

std::vector<TempReading> Sensors::Read(bool withAcpi)
{
    std::vector<TempReading> out;
    const unsigned long long now = GetTickCount64();

    // A driver update or reset invalidates adapter handles; re-enumerate a little later.
    if (m_gpuRescanAt != 0 && now >= m_gpuRescanAt)
    {
        m_gpuRescanAt = 0;
        CloseGpus();
        InitGpus();
    }
    for (size_t i = 0; i < m_gpus.size(); ++i)
    {
        TempReading r = { SENSOR_GPU, m_gpus[i].name, kNaN };
        if (m_gpus[i].nvmlDevice)
        {
            unsigned int t;
            if (g_nvmlGetTemp(m_gpus[i].nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS)
                r.celsius = t;
        }
        else
        {
            D3DKMT_ADAPTER_PERFDATA perf = {};
            if (QueryAdapter(m_gpus[i].adapter, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf)))
                r.celsius = perf.Temperature / 10.0; // deci-°C; 0 while the GPU is powered down
            else if (m_gpuRescanAt == 0)
                m_gpuRescanAt = now + kGpuRescanDelayMs;
        }
        out.push_back(r);
    }

    if (now >= m_nextDiskRead)
    {
        m_nextDiskRead = now + kDiskIntervalMs;
        for (size_t i = 0; i < m_disks.size(); ++i)
        {
            // Don't spin up a sleeping drive just to read its temperature.
            BOOL on = TRUE;
            double t;
            if (GetDevicePowerState(m_disks[i].handle, &on) && !on)
                m_disks[i].celsius = 0;
            else
                m_disks[i].celsius = QueryDiskTemp(m_disks[i].handle, t) ? t : kNaN;
        }
    }
    for (size_t i = 0; i < m_disks.size(); ++i)
    {
        TempReading r = { SENSOR_DISK, m_disks[i].name, m_disks[i].celsius };
        out.push_back(r);
    }

    if (withAcpi && !m_zonesTried)
    {
        m_zonesTried = true;
        InitThermalZones();
    }
    if (withAcpi && m_pdhQuery)
    {
        std::vector<double> values(m_zones.size(), kNaN);
        DWORD bytes = 0, count = 0;
        if (PdhCollectQueryData(m_pdhQuery) == ERROR_SUCCESS &&
            PdhGetFormattedCounterArrayW(m_pdhCounter, PDH_FMT_DOUBLE, &bytes, &count, NULL) == PDH_MORE_DATA)
        {
            std::vector<BYTE> buf(bytes);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(&buf[0]);
            if (PdhGetFormattedCounterArrayW(m_pdhCounter, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS)
            {
                for (DWORD i = 0; i < count && i < values.size(); ++i)
                    values[i] = items[i].FmtValue.doubleValue - 273.15; // counter is in Kelvin
            }
        }
        for (size_t i = 0; i < m_zones.size(); ++i)
        {
            TempReading r = { SENSOR_BOARD, L"ACPI zone " + m_zones[i], values[i] };
            out.push_back(r);
        }
    }

    return out;
}
