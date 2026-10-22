// CPU temperatures through the PawnIO driver. Opening the driver needs administrator rights,
// so this only runs inside the elevated helper process (helper.cpp).
#pragma once

#include <string>
#include <vector>

#include "sensors.h"

class CpuTemps
{
public:
    CpuTemps();
    ~CpuTemps();

    // Detects the CPUs and loads the matching PawnIO module.
    // Returns false with a user-readable message when temperatures can't be read.
    bool Open(std::wstring& error);

    // One reading per CPU package; NaN when a read failed.
    void Read(std::vector<TempReading>& out);

    // Vendor, family, model and stepping, for --dump.
    std::wstring Describe() const;

private:
    struct Thread
    {
        unsigned short group;
        unsigned long long mask; // single bit
    };

    struct Package
    {
        std::wstring name;
        std::vector<Thread> cores; // one thread per physical core; cores[0] also serves package-wide reads
        double tjMax;              // Intel: the digital sensors count down from this
        double last;               // returned while another tool holds the PCI bus
    };

    enum Kind { CPU_NONE, CPU_INTEL, CPU_AMD_ZEN, CPU_AMD_K10 };

    bool Execute(const char* function, const unsigned long long* in, size_t inCount, unsigned long long& out);
    bool ReadMsr(const Thread* on, unsigned msr, unsigned long long& value);
    bool LockPciBus();
    void UnlockPciBus();
    double IntelTjMax(const Package& p);
    double ReadIntel(const Package& p);
    double ReadAmdZen(Package& p);
    double ReadAmdK10(Package& p, size_t node);

    void* m_device;   // PawnIO handle with the module loaded
    void* m_pciMutex; // shared with other monitoring tools that touch the PCI bus
    Kind m_kind;
    std::string m_brand;
    int m_family;
    int m_model;
    int m_stepping;
    bool m_packageSensor; // Intel: IA32_PACKAGE_THERM_STATUS exists
    std::vector<Package> m_packages;

    CpuTemps(const CpuTemps&);
    CpuTemps& operator=(const CpuTemps&);
};
