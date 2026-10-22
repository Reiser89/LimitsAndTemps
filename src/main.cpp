// Lightweight tray monitor: hardware temperatures with history + AI agent usage limits.
#include <wx/wx.h>
#include <wx/config.h>
#include <wx/dcbuffer.h>
#include <wx/fontenum.h>
#include <wx/generic/stattextg.h>
#include <wx/taskbar.h>
#include <wx/snglinst.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "helper.h"
#include "sensors.h"
#include "task.h"
#include "usage.h"
#include "version.h"

namespace
{
// Sampling slows down while nobody looks; the history is time-based so the graph stays correct.
const int kVisibleIntervalMs = 2000;
const int kHiddenIntervalMs = 5000;
const unsigned long long kHistoryMs = 3 * 60 * 1000;
const unsigned long long kGapMs = 3 * kHiddenIntervalMs; // longer pauses (e.g. sleep) break the line
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Broadcast by a second launch so the running (possibly hidden) instance shows its window.
UINT ShowInstanceMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"LimitsAndTemps-ShowWindow");
    return msg;
}

// Usage limits come from the agents' own servers, so they are asked at a modest rate. Claude's status-line
// file is watched on every sample instead; it costs nothing and changes after each reply.
const unsigned long long kUsageVisibleMs = 60 * 1000;
const unsigned long long kUsageHiddenMs = 15 * 60 * 1000;
const unsigned long long kUsageLimitedMs = 30 * 60 * 1000;  // after a 429
const unsigned long long kRefreshCooldownMs = 30 * 1000;    // manual refresh
const unsigned long long kRefreshLimitedMs = 5 * 60 * 1000; // manual refresh after a 429

// A background usage fetch. The window clears `target` when it goes away, so a late answer is dropped.
struct UsageJob
{
    std::mutex lock;
    wxEvtHandler* target;
    std::vector<AgentUsage> result;
};

bool HasValue(const UsageWindow& w) { return !std::isnan(w.percent); }
int RoundPct(double pct) { return static_cast<int>(std::floor(pct + 0.5)); }

// Dark theme palette.
namespace Theme
{
wxColour Bg()        { return wxColour(24, 25, 28); }
wxColour Text()      { return wxColour(228, 230, 235); }
wxColour Muted()     { return wxColour(132, 137, 148); }
wxColour Subtle()    { return wxColour(178, 183, 193); }
wxColour Divider()   { return wxColour(48, 51, 57); }
wxColour GraphBg()   { return wxColour(16, 17, 19); }
wxColour GraphLine() { return wxColour(74, 163, 255); }
wxColour Warn()      { return wxColour(242, 184, 75); }
wxColour Bad()       { return wxColour(255, 107, 107); }
}

wxFont MonoFont(int points, bool bold)
{
    // Cascadia Mono ships with Windows 11 / Terminal; Consolas exists on every Windows 10.
    static const wxString face = wxFontEnumerator::IsValidFacename("Cascadia Mono") ? "Cascadia Mono" : "Consolas";
    wxFontInfo info(points);
    info.FaceName(face);
    if (bold)
        info.Bold();
    return wxFont(info);
}

wxColour PercentColour(int pct, int warnAt)
{
    return pct < warnAt ? Theme::Text() : pct < 90 ? Theme::Warn() : Theme::Bad();
}

wxColour TempColour(double celsius)
{
    if (std::isnan(celsius) || celsius <= 0.0)
        return Theme::Muted();
    return celsius < 70.0 ? Theme::Text() : celsius < 85.0 ? Theme::Warn() : Theme::Bad();
}

// "2h 14m", or with `clock` (week limits) "(3d 15h 1m)"; the weekday and time come from FormatResetDate.
wxString FormatReset(const wxDateTime& at, bool clock)
{
    wxTimeSpan left = at - wxDateTime::Now();
    if (left.IsNegative())
        return "resetting";
    if (!clock)
        return wxString::Format("%dh %02dm", left.GetHours(), left.GetMinutes() % 60);

    int hours = left.GetHours();
    wxString remaining = hours >= 24 ? wxString::Format("%dd ", hours / 24) : wxString();
    remaining << wxString::Format("%dh %dm", hours % 24, left.GetMinutes() % 60);
    return "(" + remaining + ")";
}

// "We, 19:30"
wxString FormatResetDate(const wxDateTime& at)
{
    static const char* const days[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" }; // fixed English, not locale
    return wxString::Format("%s, %s", days[at.GetWeekDay()], at.Format("%H:%M"));
}

// A powered-down GPU (e.g. Optimus) or spun-down drive reports 0; treat it as "no sample".
bool IsSleeping(double celsius) { return !std::isnan(celsius) && celsius <= 0.0; }

wxString FormatTemp(double celsius)
{
    if (std::isnan(celsius))
        return "n/a";
    if (IsSleeping(celsius))
        return "sleeping";
    return wxString::Format(L"%.0f°C", celsius); // no space: it reads as a gap in monospace
}

struct History
{
    struct Point
    {
        unsigned long long ms; // GetTickCount64
        double celsius;        // NaN when there was no sample
    };

    History() : max(kNaN) {}

    void Add(unsigned long long ms, double celsius)
    {
        Point p = { ms, IsSleeping(celsius) ? kNaN : celsius };
        samples.push_back(p);
        while (ms - samples.front().ms > kHistoryMs)
            samples.pop_front();
        if (!std::isnan(p.celsius) && (std::isnan(max) || p.celsius > max))
            max = p.celsius;
    }

    std::deque<Point> samples;
    double max; // highest value since the app started
};

// Small line graph of a sensor's recent history with a dashed line at its all-time max.
class Sparkline : public wxWindow
{
public:
    Sparkline(wxWindow* parent, const History* history)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(120, 18))),
          m_history(history)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &Sparkline::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(Theme::GraphBg()));
        dc.Clear();
        dc.SetPen(wxPen(Theme::Divider()));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(wxPoint(0, 0), sz);

        // Fixed scale so a climb towards the limit is visible at a glance.
        const double lo = 20.0;
        const double hi = std::max(100.0, std::isnan(m_history->max) ? 0.0 : m_history->max + 5.0);
        const int h = sz.y - 3;
        auto yOf = [&](double v) {
            double f = (std::min(std::max(v, lo), hi) - lo) / (hi - lo);
            return 1 + h - static_cast<int>(f * h + 0.5);
        };

        if (!std::isnan(m_history->max))
        {
            dc.SetPen(wxPen(Theme::Muted(), 1, wxPENSTYLE_SHORT_DASH));
            int y = yOf(m_history->max);
            dc.DrawLine(1, y, sz.x - 1, y);
        }

        const std::deque<History::Point>& s = m_history->samples;
        if (s.empty())
            return;
        // Newest sample at the right edge, kHistoryMs across the width.
        const unsigned long long newest = s.back().ms;
        auto xOf = [&](unsigned long long ms) {
            return 1 + static_cast<int>((sz.x - 2) * (1.0 - static_cast<double>(newest - ms) / kHistoryMs) + 0.5);
        };
        dc.SetPen(wxPen(Theme::GraphLine(), FromDIP(1) + 1));
        for (size_t i = 1; i < s.size(); ++i)
        {
            if (std::isnan(s[i - 1].celsius) || std::isnan(s[i].celsius) || s[i].ms - s[i - 1].ms > kGapMs)
                continue;
            dc.DrawLine(xOf(s[i - 1].ms), yOf(s[i - 1].celsius), xOf(s[i].ms), yOf(s[i].celsius));
        }
    }

    const History* m_history;
};
} // namespace

class MainFrame;

class TrayIcon : public wxTaskBarIcon
{
public:
    explicit TrayIcon(MainFrame* frame);

protected:
    wxMenu* CreatePopupMenu() override;

private:
    MainFrame* m_frame;
};

class MainFrame : public wxFrame
{
public:
    MainFrame()
        : wxFrame(NULL, wxID_ANY, "LimitsAndTemps", wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)),
          m_lastSampleMs(0),
          m_usage(InstalledAgents()),
          m_usageStartedMs(0),
          m_usageRateLimited(false),
          m_showLeft(false),
          m_statusLine(STATUSLINE_OFF),
          m_statusLineStamp(0),
          m_refresh(NULL),
          m_icon(wxICON(appicon)),
          m_panel(NULL),
          m_timer(this),
          m_tray(NULL),
          m_quitting(false)
    {
        SetIcon(m_icon);
        SetBackgroundColour(Theme::Bg());
        m_panel = new wxPanel(this);
        m_panel->SetBackgroundColour(Theme::Bg());
        m_panel->SetForegroundColour(Theme::Text());
        wxBoxSizer* frameSizer = new wxBoxSizer(wxVERTICAL);
        frameSizer->Add(m_panel, 1, wxEXPAND);
        SetSizer(frameSizer);

        m_tray = new TrayIcon(this);

        wxConfig::Get()->Read("ShowRemaining", &m_showLeft, false);
        bool enabled = false;
        wxConfig::Get()->Read("CpuSensors", &enabled, false);
        if (enabled)
            StartHelper(false);

        if (HasClaude())
        {
            // Once connected, bring the AppData copy up to this exe's version (and repair the path if needed).
            std::wstring error;
            m_statusLine = GetStatusLine();
            if (m_statusLine != STATUSLINE_OFF && ConnectStatusLine(error))
                m_statusLine = STATUSLINE_ON;
        }

        Bind(wxEVT_THREAD, &MainFrame::OnUsageFetched, this);
        Sample();

        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
        Bind(wxEVT_ICONIZE, &MainFrame::OnIconize, this);
        UpdateTimer();
    }

    ~MainFrame() override
    {
        if (m_usageJob)
        {
            std::lock_guard<std::mutex> hold(m_usageJob->lock);
            m_usageJob->target = NULL;
        }
        m_tray->RemoveIcon();
        delete m_tray;
    }

    void ShowWindow()
    {
        if (IsIconized())
            Iconize(false);
        Show(true); // back in the taskbar
        Raise();
        // Values may be a hidden interval old.
        if (GetTickCount64() - m_lastSampleMs >= static_cast<unsigned long long>(kVisibleIntervalMs))
            Sample();
        else
            UpdateWindowValues();
        UpdateTimer();
    }

    void HideToTray()
    {
        Hide(); // leaves the taskbar; the tray icon stays
        UpdateTimer();
    }

    void Quit()
    {
        m_quitting = true;
        Close(true);
    }

    static bool CpuSensorsEnabled()
    {
        bool enabled = false;
        wxConfig::Get()->Read("CpuSensors", &enabled, false);
        return enabled;
    }

    void DisableCpu()
    {
        m_helper.Stop();
        m_helperError.clear();
        wxConfig::Get()->Write("CpuSensors", false);
        // Removing the start task needs administrator rights; if declined, the task just stays unused.
        wchar_t self[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH * 4);
        if (n > 0 && n < MAX_PATH * 4)
            ShellExecuteW(NULL, L"runas", self, L"--unregister-task", NULL, SW_HIDE);
        Sample();
    }

    bool HasClaude() const
    {
        for (size_t i = 0; i < m_usage.size(); ++i)
            if (m_usage[i].claude)
                return true;
        return false;
    }

    bool StatusLineConnected() const { return m_statusLine == STATUSLINE_ON; }

    void DisconnectClaude()
    {
        std::wstring error;
        if (!DisconnectStatusLine(error))
        {
            wxMessageBox(error, "Disconnect Claude Code", wxOK | wxICON_ERROR, this);
            return;
        }
        m_statusLine = STATUSLINE_OFF;
        RebuildLayout();
    }

    // The shell tooltip is only refreshed while the pointer is over the icon,
    // so Explorer isn't asked to update it on every sample.
    void OnTrayHover()
    {
        if (m_tip == m_shownTip)
            return;
        m_tray->SetIcon(m_icon, m_tip);
        m_shownTip = m_tip;
    }

    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override
    {
        if (msg == ShowInstanceMessage())
        {
            ShowWindow();
            return 0;
        }
        return wxFrame::MSWWindowProc(msg, wParam, lParam);
    }

private:
    struct Row
    {
        wxString name;
        Sparkline* graph;
        wxStaticText* now;
        wxStaticText* max;
    };

    struct ResetLabel
    {
        wxStaticText* label;
        wxDateTime at;
        bool clock;
    };

    bool IsOnScreen() const { return IsShown() && !IsIconized(); }

    void UpdateTimer()
    {
        const int ms = IsOnScreen() ? kVisibleIntervalMs : kHiddenIntervalMs;
        if (!m_timer.IsRunning() || m_timer.GetInterval() != ms)
            m_timer.Start(ms);
    }

    // CPUs from the helper, then GPUs and drives, then ACPI zones as a CPU fallback.
    std::vector<TempReading> Collect()
    {
        std::vector<TempReading> out;
        m_helper.Read(out, m_helperMessage);
        const bool cpuFromHelper = m_helper.IsRunning() && m_helperMessage.empty();

        std::vector<TempReading> native = m_sensors.Read(!cpuFromHelper);
        for (size_t i = 0; i < native.size(); ++i)
        {
            const TempReading& r = native[i];
            // Skip GPUs that never reported a temperature (most iGPUs, or a dGPU asleep since start).
            if (r.kind == SENSOR_GPU && !(r.celsius > 0.0) && m_history.find(r.name) == m_history.end())
                continue;
            out.push_back(r);
        }
        return out;
    }

    void Sample()
    {
        m_readings = Collect();
        const unsigned long long now = GetTickCount64();
        m_lastSampleMs = now;
        for (size_t i = 0; i < m_readings.size(); ++i)
            m_history[m_readings[i].name].Add(now, m_readings[i].celsius);

        bool rowsChanged = m_readings.size() != m_rows.size();
        for (size_t i = 0; !rowsChanged && i < m_readings.size(); ++i)
            rowsChanged = m_rows[i].name != wxString(m_readings[i].name);
        if (rowsChanged || m_shownHelperMessage != HelperMessage())
            RebuildLayout();

        if (IsOnScreen())
            UpdateWindowValues();
        m_tip = TrayTooltip();
        if (!m_tray->IsIconInstalled())
            OnTrayHover();

        CheckStatusLine();
        if (UsageDue(now))
            StartUsageFetch();
    }

    static bool SameWindow(const UsageWindow& a, const UsageWindow& b)
    {
        return HasValue(a) == HasValue(b) && (!HasValue(a) || a.percent == b.percent) && a.resetsAt == b.resetsAt;
    }

    // Claude Code rewrites the status-line file after each reply; only its timestamp is checked per sample.
    void CheckStatusLine()
    {
        const unsigned long long stamp = StatusLineStamp();
        if (stamp == m_statusLineStamp)
            return;
        m_statusLineStamp = stamp;
        for (size_t i = 0; i < m_usage.size(); ++i)
        {
            AgentUsage fresh = m_usage[i];
            if (!fresh.claude || !ReadStatusLine(fresh))
                continue;
            const AgentUsage& old = m_usage[i];
            const bool same = old.error.empty() && SameWindow(old.fiveHour, fresh.fiveHour) &&
                              SameWindow(old.week, fresh.week) && SameWindow(old.fableWeek, fresh.fableWeek);
            m_usage[i] = fresh;
            m_usageUpdated = wxDateTime::Now();
            if (same)
                continue;
            RebuildLayout();
            m_tip = TrayTooltip();
            if (!m_tray->IsIconInstalled())
                OnTrayHover();
        }
    }

    bool UsageDue(unsigned long long now) const
    {
        if (m_usage.empty() || m_usageJob)
            return false;
        if (m_usageStartedMs == 0)
            return true;
        const unsigned long long wait = m_usageRateLimited ? kUsageLimitedMs : IsOnScreen() ? kUsageVisibleMs : kUsageHiddenMs;
        return now - m_usageStartedMs >= wait;
    }

    bool CanRefreshUsage() const
    {
        const unsigned long long cooldown = m_usageRateLimited ? kRefreshLimitedMs : kRefreshCooldownMs;
        return !m_usageJob && (m_usageStartedMs == 0 || GetTickCount64() - m_usageStartedMs >= cooldown);
    }

    void StartUsageFetch()
    {
        m_usageStartedMs = GetTickCount64();
        std::shared_ptr<UsageJob> job = std::make_shared<UsageJob>();
        job->target = this;
        m_usageJob = job;
        if (m_refresh)
            m_refresh->Disable();
        // Detached so that closing never waits for the network; see UsageJob.
        std::thread([job] {
            std::vector<AgentUsage> result = FetchUsage();
            std::lock_guard<std::mutex> hold(job->lock);
            job->result.swap(result);
            if (job->target)
                wxQueueEvent(job->target, new wxThreadEvent());
        }).detach();
    }

    void OnUsageFetched(wxThreadEvent&)
    {
        if (!m_usageJob)
            return;
        std::vector<AgentUsage> fresh;
        {
            std::lock_guard<std::mutex> hold(m_usageJob->lock);
            fresh.swap(m_usageJob->result);
        }
        m_usageJob.reset();

        m_usageRateLimited = false;
        for (size_t i = 0; i < fresh.size(); ++i)
        {
            AgentUsage& a = fresh[i];
            m_usageRateLimited = m_usageRateLimited || a.rateLimited;
            if (a.error.empty())
                continue;
            // A failed fetch keeps the last known values; the error line marks them as possibly stale.
            for (size_t j = 0; j < m_usage.size(); ++j)
                if (m_usage[j].name == a.name)
                {
                    a.fiveHour = m_usage[j].fiveHour;
                    a.week = m_usage[j].week;
                    a.fableWeek = m_usage[j].fableWeek;
                }
        }
        bool anyGood = false;
        for (size_t i = 0; i < fresh.size(); ++i)
            anyGood = anyGood || fresh[i].error.empty();
        if (anyGood)
            m_usageUpdated = wxDateTime::Now();
        m_usage.swap(fresh);
        if (HasClaude())
            m_statusLine = GetStatusLine(); // settings.json may have been edited meanwhile

        RebuildLayout();
        m_tip = TrayTooltip();
        if (!m_tray->IsIconInstalled())
            OnTrayHover();
    }

    void OnRefreshUsage(wxCommandEvent&)
    {
        if (CanRefreshUsage())
            StartUsageFetch();
    }

    wxString HelperMessage() const
    {
        if (!m_helperError.empty())
            return m_helperError;
        return m_helperMessage;
    }

    void RebuildLayout()
    {
        Freeze();
        m_panel->DestroyChildren();
        m_rows.clear();
        m_resetLabels.clear();
        m_refresh = NULL;
        m_shownHelperMessage = HelperMessage();

        wxFlexGridSizer* grid = new wxFlexGridSizer(4, FromDIP(6), FromDIP(14));
        grid->AddGrowableCol(0, 1);

        AddHeader(grid, "Temperatures");
        grid->AddSpacer(0);
        AddRight(grid, MutedText("Now"));
        AddRight(grid, MutedText("Max"));

        for (size_t i = 0; i < m_readings.size(); ++i)
        {
            Row row;
            row.name = m_readings[i].name;
            // Names come from drivers and can be arbitrarily long; clip the label so the layout stays stable.
            const size_t kMaxNameChars = 36;
            wxString shown = row.name.length() > kMaxNameChars ? row.name.Left(kMaxNameChars - 1) + L"…" : row.name;
            wxStaticText* nameLabel = new wxStaticText(m_panel, wxID_ANY, shown);
            if (shown != row.name)
                nameLabel->SetToolTip(row.name);
            grid->Add(nameLabel, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
            row.graph = new Sparkline(m_panel, &m_history[m_readings[i].name]);
            grid->Add(row.graph, 0, wxALIGN_CENTER_VERTICAL);
            row.now = new wxStaticText(m_panel, wxID_ANY, "");
            row.now->SetFont(MonoFont(10, true));
            row.max = new wxStaticText(m_panel, wxID_ANY, "");
            row.max->SetFont(MonoFont(10, false));
            AddRight(grid, row.now);
            AddRight(grid, row.max);
            m_rows.push_back(row);
        }
        if (m_readings.empty())
        {
            grid->Add(new wxStaticText(m_panel, wxID_ANY, "No sensors found"), 0, wxLEFT, FromDIP(10));
            grid->AddSpacer(0);
            grid->AddSpacer(0);
            grid->AddSpacer(0);
        }

        wxBoxSizer* helperLine = NULL;
        if (!m_helper.IsRunning() || !m_shownHelperMessage.empty())
        {
            helperLine = new wxBoxSizer(wxHORIZONTAL);
            wxButton* button = new wxButton(m_panel, wxID_ANY,
                                       m_helper.IsRunning() || !m_helperError.empty() ? "Retry CPU sensors" : "Enable CPU temperature...");
            button->Bind(wxEVT_BUTTON, &MainFrame::OnEnableCpu, this);
            helperLine->Add(button, 0, wxALIGN_CENTER_VERTICAL);
            helperLine->Add(MutedText(m_shownHelperMessage), 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
        }

        wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
        top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
        if (helperLine)
            top->Add(helperLine, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
        if (!m_usage.empty())
        {
            wxPanel* divider = new wxPanel(m_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            divider->SetBackgroundColour(Theme::Divider());
            top->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
            top->Add(AgentLimits(), 0, wxEXPAND | wxALL, FromDIP(14));
        }
        else
            top->AddSpacer(FromDIP(14));
        m_panel->SetSizer(top);

        UpdateWindowValues();
        m_panel->Layout();
        GetSizer()->SetSizeHints(this);
        Fit();
        Thaw();
    }

    // Agent limits: one column per limit, big monospace value with its reset time underneath.
    wxSizer* AgentLimits()
    {
        bool anyFable = false;
        for (size_t i = 0; i < m_usage.size(); ++i)
            anyFable = anyFable || HasValue(m_usage[i].fableWeek);

        wxFlexGridSizer* agents = new wxFlexGridSizer(anyFable ? 4 : 3, FromDIP(10), FromDIP(22));
        agents->AddGrowableCol(0, 1);

        wxBoxSizer* title = new wxBoxSizer(wxHORIZONTAL);
        AddHeader(title, "AI agent limits");
        m_refresh = new wxButton(m_panel, wxID_ANY, L"↻", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        m_refresh->SetFont(wxFont(wxFontInfo(10).FaceName("Segoe UI Symbol")));
        m_refresh->SetToolTip(m_usageUpdated.IsValid() ? "Refresh (updated " + m_usageUpdated.Format("%H:%M") + ")"
                                                      : wxString("Refresh"));
        m_refresh->Enable(CanRefreshUsage());
        m_refresh->Bind(wxEVT_BUTTON, &MainFrame::OnRefreshUsage, this);
        title->Add(m_refresh, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
        // Generic so it gets mouse clicks (the native static control lets them through to the parent).
        wxGenericStaticText* mode = new wxGenericStaticText(m_panel, wxID_ANY, m_showLeft ? "% left" : "% used");
        mode->SetForegroundColour(Theme::Subtle());
        mode->SetBackgroundColour(Theme::Bg());
        mode->SetCursor(wxCursor(wxCURSOR_HAND));
        mode->SetToolTip(m_showLeft ? "Remaining limits. Click to show used." : "Used limits. Click to show remaining.");
        mode->Bind(wxEVT_LEFT_UP, &MainFrame::OnFlipLimits, this);
        title->Add(mode, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
        agents->Add(title, 0, wxALIGN_BOTTOM);
        AddRight(agents, MutedText("5h"));
        AddRight(agents, MutedText("1w"));
        if (anyFable)
            AddRight(agents, MutedText("Fable 1w"));

        for (size_t i = 0; i < m_usage.size(); ++i)
        {
            const AgentUsage& a = m_usage[i];
            wxBoxSizer* name = new wxBoxSizer(wxVERTICAL);
            name->Add(new wxStaticText(m_panel, wxID_ANY, a.name));
            if (!a.error.empty())
            {
                wxStaticText* error = MutedText(a.error);
                error->SetFont(error->GetFont().Scaled(0.85f));
                name->Add(error);
            }
            if (a.claude && !a.error.empty() && !StatusLineConnected())
            {
                wxButton* connect = new wxButton(m_panel, wxID_ANY, "Connect Claude Code...");
                connect->Bind(wxEVT_BUTTON, &MainFrame::OnConnectClaude, this);
                name->Add(connect, 0, wxTOP, FromDIP(4));
            }
            agents->Add(name, 0, wxLEFT | wxTOP, FromDIP(10));
            AddLimit(agents, a.fiveHour, 50, false);
            AddLimit(agents, a.week, 75, true);
            if (anyFable)
            {
                // Shares the week reset, so it has no reset line of its own.
                UsageWindow fable = { a.fableWeek.percent, 0 };
                AddLimit(agents, fable, 75, false);
            }
        }
        return agents;
    }

    void AddLimit(wxSizer* grid, const UsageWindow& w, int warnAt, bool clock)
    {
        if (!HasValue(w))
        {
            AddRight(grid, MutedText("-"));
            return;
        }
        wxDateTime reset = w.resetsAt > 0 ? wxDateTime(static_cast<time_t>(w.resetsAt)) : wxInvalidDateTime;
        grid->Add(LimitCell(RoundPct(w.percent), warnAt, reset, clock), 0, wxALIGN_RIGHT);
    }

    void AddHeader(wxSizer* grid, const wxString& text)
    {
        wxStaticText* label = new wxStaticText(m_panel, wxID_ANY, text);
        label->SetFont(label->GetFont().MakeBold().Scaled(1.1f));
        grid->Add(label, 0, wxBOTTOM | wxALIGN_BOTTOM, FromDIP(2));
    }

    wxStaticText* MutedText(const wxString& text)
    {
        wxStaticText* label = new wxStaticText(m_panel, wxID_ANY, text);
        label->SetForegroundColour(Theme::Muted());
        return label;
    }

    // Percentage in a large coloured monospace font, plus a small reset line when `resetAt` is valid.
    // The colour always follows the used part, also while the remaining part is shown.
    wxSizer* LimitCell(int pct, int warnAt, const wxDateTime& resetAt, bool clock = false)
    {
        wxBoxSizer* cell = new wxBoxSizer(wxVERTICAL);
        wxStaticText* value = new wxStaticText(m_panel, wxID_ANY, wxString::Format("%3d%%", Shown(pct)));
        value->SetFont(MonoFont(15, true));
        value->SetForegroundColour(PercentColour(pct, warnAt));
        cell->Add(value, 0, wxALIGN_RIGHT);
        if (resetAt.IsValid())
        {
            wxStaticText* reset = MutedText(FormatReset(resetAt, clock));
            wxFont resetFont = reset->GetFont().Scaled(0.85f);
            if (clock)
                resetFont.SetWeight(wxFONTWEIGHT_LIGHT);
            reset->SetFont(resetFont);
            if (clock)
            {
                // Weekday and time in a lighter colour than the countdown next to it.
                wxStaticText* date = new wxStaticText(m_panel, wxID_ANY, FormatResetDate(resetAt));
                date->SetForegroundColour(Theme::Subtle());
                date->SetFont(resetFont);
                wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
                line->Add(date, 0, wxRIGHT, FromDIP(4));
                line->Add(reset, 0);
                cell->Add(line, 0, wxALIGN_RIGHT);
            }
            else
                cell->Add(reset, 0, wxALIGN_RIGHT);
            ResetLabel r = { reset, resetAt, clock };
            m_resetLabels.push_back(r);
        }
        return cell;
    }

    int Shown(int usedPct) const { return m_showLeft ? std::max(0, 100 - usedPct) : usedPct; }

    void OnFlipLimits(wxMouseEvent&)
    {
        m_showLeft = !m_showLeft;
        wxConfig::Get()->Write("ShowRemaining", m_showLeft);
        m_tip = TrayTooltip();
        CallAfter([this] { RebuildLayout(); }); // not while the clicked label handles its event
    }

    void AddRight(wxSizer* grid, wxWindow* w)
    {
        grid->Add(w, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    }

    static bool SetLabelIfChanged(wxStaticText* label, const wxString& text)
    {
        if (label->GetLabel() == text)
            return false;
        label->SetLabel(text);
        return true;
    }

    void UpdateWindowValues()
    {
        bool changed = false;
        for (size_t i = 0; i < m_rows.size() && i < m_readings.size(); ++i)
        {
            const History& h = m_history[m_readings[i].name];
            if (SetLabelIfChanged(m_rows[i].now, FormatTemp(m_readings[i].celsius)))
                changed = true;
            m_rows[i].now->SetForegroundColour(TempColour(m_readings[i].celsius));
            if (SetLabelIfChanged(m_rows[i].max, std::isnan(h.max) ? wxString("-") : FormatTemp(h.max)))
                changed = true;
            m_rows[i].max->SetForegroundColour(TempColour(h.max));
            m_rows[i].graph->Refresh(false);
        }
        for (size_t i = 0; i < m_resetLabels.size(); ++i)
            if (SetLabelIfChanged(m_resetLabels[i].label, FormatReset(m_resetLabels[i].at, m_resetLabels[i].clock)))
                changed = true;
        if (m_refresh)
            m_refresh->Enable(CanRefreshUsage()); // the cooldown ends without any other event
        if (changed)
            m_panel->Layout();
    }

    wxString TrayTooltip() const
    {
        wxString tip = "LimitsAndTemps";
        for (size_t i = 0; i < m_readings.size(); ++i)
            tip << "\n" << wxString(m_readings[i].name).Left(20) << ": " << FormatTemp(m_readings[i].celsius);
        for (size_t i = 0; i < m_usage.size(); ++i)
        {
            const AgentUsage& a = m_usage[i];
            if (!HasValue(a.fiveHour) && !HasValue(a.week))
                continue;
            tip << "\n" << a.name;
            if (HasValue(a.fiveHour))
                tip << wxString::Format(" 5h %d%%", Shown(RoundPct(a.fiveHour.percent)));
            if (HasValue(a.week))
                tip << wxString::Format(" 1w %d%%", Shown(RoundPct(a.week.percent)));
            if (HasValue(a.fableWeek))
                tip << wxString::Format(" F %d%%", Shown(RoundPct(a.fableWeek.percent)));
            tip << (m_showLeft ? " left" : " used");
        }
        return tip.Left(127); // shell tooltip limit
    }

    void StartHelper(bool installPawnIo)
    {
        std::wstring error;
        m_helperError = m_helper.Start(installPawnIo, error) ? wxString() : wxString(error);
    }

    void OnEnableCpu(wxCommandEvent&)
    {
        if (!CpuSensorsEnabled())
        {
            wxString install = HelperClient::IsPawnIoInstalled()
                                   ? wxString("The PawnIO driver is already installed.\n")
                                   : wxString("PawnIO is not installed yet. Its official installer is built in and will run now, without further questions.\n");
            int answer = wxMessageBox(
                "Reading CPU temperatures needs administrator rights and the PawnIO driver "
                "(open source, signed, used by LibreHardwareMonitor and FanControl).\n\n" + install +
                "Windows asks for permission once. LimitsAndTemps then copies itself to Program Files and adds "
                "a scheduled task, so later starts need no prompt. \"Disable CPU temperature\" in the tray menu "
                "removes both.\n\nContinue?",
                "Enable CPU temperature", wxOK | wxCANCEL | wxICON_INFORMATION, this);
            if (answer != wxOK)
                return;
        }

        StartHelper(!HelperClient::IsPawnIoInstalled());
        if (m_helperError.empty())
            wxConfig::Get()->Write("CpuSensors", true);
        CallAfter([this] { Sample(); });
    }

    void OnConnectClaude(wxCommandEvent&)
    {
        int answer = wxMessageBox(
            "Claude Code shares its usage limits with its status-line command after every reply. "
            "LimitsAndTemps will register itself as that command in Claude Code's settings.json and keep "
            "the limits it receives. Nothing is sent anywhere.\n\n"
            "A status line you already use keeps working: it runs behind LimitsAndTemps and gets the same input.\n\n"
            "Limits appear after Claude Code's next reply. \"Disconnect Claude Code\" in the tray menu "
            "restores the previous setting.\n\nContinue?",
            "Connect Claude Code", wxOK | wxCANCEL | wxICON_INFORMATION, this);
        if (answer != wxOK)
            return;
        std::wstring error;
        if (!ConnectStatusLine(error))
        {
            wxMessageBox(error, "Connect Claude Code", wxOK | wxICON_ERROR, this);
            return;
        }
        m_statusLine = STATUSLINE_ON;
        for (size_t i = 0; i < m_usage.size(); ++i)
            if (m_usage[i].claude && !HasValue(m_usage[i].fiveHour) && !HasValue(m_usage[i].week))
                m_usage[i].error = L"waiting for Claude Code's next reply";
        CallAfter([this] { RebuildLayout(); }); // not while the clicked button handles its event
    }

    void OnTimer(wxTimerEvent&)
    {
        Sample();
    }

    void OnIconize(wxIconizeEvent& event)
    {
        event.Skip();
        UpdateTimer();
        if (!event.IsIconized())
            Sample();
    }

    void OnClose(wxCloseEvent& event)
    {
        if (!m_quitting && event.CanVeto())
        {
            event.Veto();
            HideToTray();
            return;
        }
        m_helper.Stop();
        Destroy();
    }

    Sensors m_sensors;
    HelperClient m_helper;
    wxString m_helperError;
    std::wstring m_helperMessage;
    wxString m_shownHelperMessage;

    std::vector<TempReading> m_readings;
    std::map<std::wstring, History> m_history;
    unsigned long long m_lastSampleMs;
    std::vector<Row> m_rows;
    std::vector<ResetLabel> m_resetLabels;

    std::vector<AgentUsage> m_usage;         // installed agents only
    std::shared_ptr<UsageJob> m_usageJob;    // the fetch in flight, if any
    unsigned long long m_usageStartedMs;     // GetTickCount64 of the last fetch start; 0 = never
    bool m_usageRateLimited;
    wxDateTime m_usageUpdated;               // last answer with fresh values
    bool m_showLeft;                         // limits shown as remaining instead of used
    StatusLineState m_statusLine;            // Claude Code's status line points at our AppData copy
    unsigned long long m_statusLineStamp;    // last seen write of its saved limits
    wxButton* m_refresh;

    wxIcon m_icon;
    wxString m_tip;
    wxString m_shownTip;
    wxPanel* m_panel;
    wxTimer m_timer;
    TrayIcon* m_tray;
    bool m_quitting;
};

TrayIcon::TrayIcon(MainFrame* frame)
    : m_frame(frame)
{
    Bind(wxEVT_TASKBAR_LEFT_UP, [this](wxTaskBarIconEvent&) { m_frame->ShowWindow(); });
    Bind(wxEVT_TASKBAR_MOVE, [this](wxTaskBarIconEvent&) { m_frame->OnTrayHover(); });
}

wxMenu* TrayIcon::CreatePopupMenu()
{
    wxMenu* menu = new wxMenu;
    menu->Append(wxID_ANY, "LimitsAndTemps " APP_VERSION_STR)->Enable(false);
    menu->AppendSeparator();
    menu->Append(wxID_OPEN, "Show");
    if (MainFrame::CpuSensorsEnabled())
    {
        wxMenuItem* disable = menu->Append(wxID_ANY, "Disable CPU temperature");
        menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_frame->DisableCpu(); }, disable->GetId());
    }
    if (m_frame->StatusLineConnected())
    {
        wxMenuItem* disconnect = menu->Append(wxID_ANY, "Disconnect Claude Code");
        menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_frame->DisconnectClaude(); }, disconnect->GetId());
    }
    menu->AppendSeparator();
    menu->Append(wxID_EXIT, "Exit");
    menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_frame->ShowWindow(); }, wxID_OPEN);
    menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_frame->Quit(); }, wxID_EXIT);
    return menu;
}

class LimitsAndTempsApp : public wxApp
{
public:
    bool OnInit() override
    {
        m_instance.Create("LimitsAndTemps-" + wxGetUserId());
        if (m_instance.IsAnotherRunning())
        {
            // Already running (maybe only in the tray): bring that window up instead.
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(HWND_BROADCAST, ShowInstanceMessage(), 0, 0);
            return false;
        }

        SetAppName("LimitsAndTemps"); // settings live in HKCU\Software\LimitsAndTemps
        MSWEnableDarkMode(DarkMode_Always); // dark title bar, buttons and menus (Win10 1809+)

        bool startHidden = false;
        for (int i = 1; i < argc; ++i)
            if (argv[i] == "--tray")
                startHidden = true;

        MainFrame* frame = new MainFrame();
        if (startHidden)
            frame->HideToTray();
        else
            frame->ShowWindow();
        return true;
    }

private:
    wxSingleInstanceChecker m_instance;
};

wxIMPLEMENT_APP_NO_MAIN(LimitsAndTempsApp);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prevInstance, PWSTR, int show)
{
    // The elevated helper and the command-line modes have no UI, so they never initialize wx.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--helper") == 0)
        return RunHelper(argc, argv);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--unregister-task") == 0)
    {
        HelperTask::Unregister();
        return 0;
    }
    if (argv && argc >= 3 && wcscmp(argv[1], L"--dump") == 0)
        return RunDump(argv[2]);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--statusline") == 0)
        return RunStatusLine(argc >= 3 && wcscmp(argv[2], L"--pass") == 0);
    LocalFree(argv);
    return wxEntry(instance, prevInstance, NULL, show);
}
