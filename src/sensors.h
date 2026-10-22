// Hardware temperature sources that work without admin rights or a kernel driver.
#pragma once

#include <string>
#include <vector>

enum SensorKind { SENSOR_CPU, SENSOR_GPU, SENSOR_DISK, SENSOR_BOARD };

struct TempReading
{
    SensorKind kind;
    std::wstring name;
    double celsius; // NaN when the sensor could not be read; 0 when the device sleeps
};

class Sensors
{
public:
    Sensors();
    ~Sensors();

    // Returns GPUs and drives discovered at construction, plus ACPI thermal zones when
    // withAcpi is set (they are only worth their cost when there is no CPU reading).
    std::vector<TempReading> Read(bool withAcpi);

private:
    struct Disk { void* handle; std::wstring name; double celsius; };
    struct Gpu  { unsigned adapter; void* nvmlDevice; std::wstring name; };

    void InitGpus();
    void CloseGpus();
    void InitNvml();
    void InitDisks();
    void InitThermalZones();

    // GPUs: D3DKMT adapter perf data (what Task Manager shows), NVML only for old NVIDIA drivers
    std::vector<Gpu> m_gpus;
    void* m_nvml;
    unsigned long long m_gpuRescanAt; // GetTickCount64 time; 0 = no rescan pending

    // NVMe / SATA drives reporting temperature
    std::vector<Disk> m_disks;
    unsigned long long m_nextDiskRead;

    // ACPI thermal zones (performance counters), opened on first use
    bool m_zonesTried;
    void* m_pdhQuery;
    void* m_pdhCounter;
    std::vector<std::wstring> m_zones;

    Sensors(const Sensors&);
    Sensors& operator=(const Sensors&);
};
