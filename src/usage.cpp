#include "usage.h"

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <utility>

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const wchar_t kClaude[] = L"Claude Code";
const wchar_t kCodex[] = L"Codex";

// Just enough JSON for the credential files and usage answers.
struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : type(Null), number(kNaN), begin(0), end(0), keyBegin(0) {}

    const Json& operator[](const char* key) const
    {
        static const Json missing;
        for (size_t i = 0; i < members.size(); ++i)
            if (members[i].first == key)
                return members[i].second;
        return missing;
    }

    bool Has(const char* key) const
    {
        for (size_t i = 0; i < members.size(); ++i)
            if (members[i].first == key)
                return true;
        return false;
    }

    double Num() const { return type == Number ? number : kNaN; }

    Type type;
    double number;
    std::string string;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json> > members;
    // Byte range of the value in the parsed text, so a file can be edited without reformatting it.
    size_t begin, end;
    size_t keyBegin; // object members: where the key starts
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& text, size_t start = 0)
        : m_base(text.c_str()), m_p(text.c_str() + start), m_end(text.c_str() + text.size()) {}

    bool Parse(Json& out)
    {
        bool ok = Value(out, 0);
        Skip();
        return ok && m_p == m_end;
    }

private:
    void Skip()
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
            ++m_p;
    }

    bool Literal(const char* s)
    {
        size_t n = strlen(s);
        if (static_cast<size_t>(m_end - m_p) < n || memcmp(m_p, s, n) != 0)
            return false;
        m_p += n;
        return true;
    }

    bool Value(Json& v, int depth)
    {
        Skip();
        v.begin = m_p - m_base;
        bool ok = Body(v, depth);
        v.end = m_p - m_base;
        return ok;
    }

    bool Body(Json& v, int depth)
    {
        if (m_p >= m_end || depth > 32)
            return false;
        switch (*m_p)
        {
        case '{':
            v.type = Json::Object;
            ++m_p;
            Skip();
            if (m_p < m_end && *m_p == '}')
                return ++m_p, true;
            for (;;)
            {
                Skip();
                const size_t keyAt = m_p - m_base;
                std::string key;
                if (m_p >= m_end || *m_p != '"' || !Str(key))
                    return false;
                Skip();
                if (m_p >= m_end || *m_p++ != ':')
                    return false;
                v.members.push_back(std::make_pair(key, Json()));
                v.members.back().second.keyBegin = keyAt;
                if (!Value(v.members.back().second, depth + 1))
                    return false;
                Skip();
                if (m_p < m_end && *m_p == ',')
                    ++m_p;
                else if (m_p < m_end && *m_p == '}')
                    return ++m_p, true;
                else
                    return false;
            }
        case '[':
            v.type = Json::Array;
            ++m_p;
            Skip();
            if (m_p < m_end && *m_p == ']')
                return ++m_p, true;
            for (;;)
            {
                v.items.push_back(Json());
                if (!Value(v.items.back(), depth + 1))
                    return false;
                Skip();
                if (m_p < m_end && *m_p == ',')
                    ++m_p;
                else if (m_p < m_end && *m_p == ']')
                    return ++m_p, true;
                else
                    return false;
            }
        case '"':
            v.type = Json::String;
            return Str(v.string);
        case 't':
            v.type = Json::Bool;
            return Literal("true");
        case 'f':
            v.type = Json::Bool;
            return Literal("false");
        case 'n':
            return Literal("null");
        default:
            v.type = Json::Number;
            return Number(v.number);
        }
    }

    // By hand rather than strtod, which follows the C locale's decimal separator.
    bool Number(double& out)
    {
        const char* start = m_p;
        bool negative = m_p < m_end && *m_p == '-';
        if (negative)
            ++m_p;
        double value = 0.0;
        while (m_p < m_end && *m_p >= '0' && *m_p <= '9')
            value = value * 10.0 + (*m_p++ - '0');
        if (m_p < m_end && *m_p == '.')
        {
            double scale = 0.1;
            for (++m_p; m_p < m_end && *m_p >= '0' && *m_p <= '9'; ++m_p, scale /= 10.0)
                value += (*m_p - '0') * scale;
        }
        if (m_p < m_end && (*m_p == 'e' || *m_p == 'E'))
        {
            ++m_p;
            bool negExp = m_p < m_end && *m_p == '-';
            if (m_p < m_end && (*m_p == '-' || *m_p == '+'))
                ++m_p;
            int exp = 0;
            while (m_p < m_end && *m_p >= '0' && *m_p <= '9' && exp < 400)
                exp = exp * 10 + (*m_p++ - '0');
            value *= std::pow(10.0, negExp ? -exp : exp);
        }
        out = negative ? -value : value;
        return m_p > start + (negative ? 1 : 0);
    }

    bool Str(std::string& out)
    {
        ++m_p; // opening quote
        while (m_p < m_end)
        {
            char c = *m_p++;
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (m_p >= m_end)
                return false;
            char e = *m_p++;
            switch (e)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                if (m_end - m_p < 4)
                    return false;
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char h = *m_p++;
                    cp = cp * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                // None of the values read here need characters outside the BMP; surrogates become '?'.
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    out += '?';
                else if (cp < 0x80)
                    out += static_cast<char>(cp);
                else if (cp < 0x800)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    const char* m_base;
    const char* m_p;
    const char* m_end;
};

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0);
    std::wstring out(n, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::wstring Env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf) : std::wstring();
}

// The agents' own override variable, else their folder in the user profile.
std::wstring ConfigDir(const wchar_t* overrideVar, const wchar_t* homeSubdir)
{
    std::wstring dir = Env(overrideVar);
    if (!dir.empty())
        return dir;
    std::wstring home = Env(L"USERPROFILE");
    return home.empty() ? home : home + L"\\" + homeSubdir;
}

std::wstring ClaudeDir() { return ConfigDir(L"CLAUDE_CONFIG_DIR", L".claude"); }
std::wstring CodexDir()  { return ConfigDir(L"CODEX_HOME", L".codex"); }

bool IsDir(const std::wstring& path)
{
    DWORD attr = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Narrow(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0, NULL, NULL);
    std::string out(n, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, NULL, NULL);
    return out;
}

bool IsFile(const std::wstring& path)
{
    DWORD attr = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadTextFile(const std::wstring& path, std::string& text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    text.clear();
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 1024 * 1024;
    if (ok && size.QuadPart > 0)
    {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &read, NULL) != FALSE;
        text.resize(ok ? read : 0);
    }
    CloseHandle(f);
    return ok;
}

bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, NULL) && written == text.size();
    CloseHandle(f);
    return ok;
}

// Offsets stay relative to the whole text, also behind a UTF-8 BOM.
bool ParseJson(const std::string& text, Json& out)
{
    return JsonParser(text, text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0).Parse(out);
}

// Re-read on every fetch: the agents renew their tokens themselves and rewrite the file.
bool ReadJsonFile(const std::wstring& path, Json& out)
{
    std::string text;
    return ReadTextFile(path, text) && ParseJson(text, out);
}

std::string JsonQuote(const std::string& s)
{
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\')
            out += '\\';
        if (c < 0x20)
        {
            char esc[8];
            sprintf_s(esc, "\\u%04x", c);
            out += esc;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

class Handle
{
public:
    explicit Handle(HINTERNET h) : m_h(h) {}
    ~Handle() { if (m_h) WinHttpCloseHandle(m_h); }
    operator HINTERNET() const { return m_h; }

private:
    HINTERNET m_h;
    Handle(const Handle&);
    Handle& operator=(const Handle&);
};

// One HTTPS GET. Returns the HTTP status, or 0 when there was no answer.
DWORD HttpGet(const wchar_t* host, const wchar_t* path, const std::wstring& headers, std::string& body)
{
    Handle session(WinHttpOpen(L"LimitsAndTemps", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return 0;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    Handle connect(WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect)
        return 0;
    Handle request(WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE));
    if (!request)
        return 0;
    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL))
        return 0;

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    char buf[8192];
    DWORD read = 0;
    while (body.size() < 1024 * 1024 && WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0)
        body.append(buf, read);
    return status;
}

// Shared handling of non-200 answers; true when the body is worth parsing.
bool CheckStatus(DWORD status, const wchar_t* agent, AgentUsage& usage)
{
    if (status == 200)
        return true;
    if (status == 0)
        usage.error = L"no connection";
    else if (status == 401 || status == 403)
        usage.error = std::wstring(L"sign-in expired, open ") + agent + L" to renew it";
    else if (status == 429)
    {
        usage.error = L"rate limited, retrying later";
        usage.rateLimited = true;
    }
    else
        usage.error = L"server error " + std::to_wstring(status);
    return false;
}

// "2026-10-22T19:30:00.123456+00:00" or "...Z" -> Unix seconds, 0 if unreadable.
long long ParseIsoTime(const std::string& s)
{
    int y, mo, d, h, mi, sec;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    long long unix = _mkgmtime64(&t);
    if (unix < 0)
        return 0;
    size_t zone = s.find_first_of("+-Z", 19);
    if (zone != std::string::npos && s[zone] != 'Z')
    {
        int zh = 0, zm = 0;
        sscanf_s(s.c_str() + zone + 1, "%d:%d", &zh, &zm);
        unix += (s[zone] == '+' ? -1 : 1) * (zh * 3600LL + zm * 60LL);
    }
    return unix;
}

AgentUsage Empty(const wchar_t* name)
{
    UsageWindow none = { kNaN, 0 };
    AgentUsage a = { name, none, none, none, std::wstring(), false, wcscmp(name, kClaude) == 0 };
    return a;
}

// Claude Code passes `rate_limits` to its status-line command after every reply. Registered as that command,
// this exe saves them here, so Claude's limits need no request and no readable sign-in (Claude Code inside
// the desktop app keeps its sign-in to itself, so .credentials.json goes stale).
const wchar_t kStatusLineFile[] = L"\\limitsandtemps-usage.json";
const char kStatusLineFlag[] = " --statusline";
const char kStatusLinePass[] = " --statusline --pass | "; // an existing status line keeps running behind ours
const unsigned long long kFreshStatusLine = 60ULL * 10000000; // FILETIME units

std::wstring StatusLinePath() { return ClaudeDir() + kStatusLineFile; }

// Claude Code runs a copy in the user's AppData, so moving or deleting the exe the user started doesn't
// break the status line.
std::wstring StatusLineExe()
{
    std::wstring base = Env(L"LOCALAPPDATA");
    return base.empty() ? base : base + L"\\LimitsAndTemps\\LimitsAndTemps.exe";
}

// Forward slashes and quotes work in both shells Claude Code runs commands with (cmd, Git Bash).
std::string CommandPath(const std::wstring& path)
{
    std::string out = Narrow(path);
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '\\')
            out[i] = '/';
    return out;
}

// CopyFile keeps the write time, so size and write time tell whether the copy matches this exe.
bool SameFile(const std::wstring& a, const std::wstring& b)
{
    WIN32_FILE_ATTRIBUTE_DATA da, db;
    return GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &da) &&
           GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &db) && da.nFileSizeLow == db.nFileSizeLow &&
           da.nFileSizeHigh == db.nFileSizeHigh && CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime) == 0;
}

// Puts this exe at StatusLineExe(). A copy Claude Code is running right now can't be overwritten but can be
// renamed, so the old one is moved aside first and deleted on a later update.
bool InstallStatusLineExe(std::wstring& error)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    const std::wstring self = n == 0 || n >= MAX_PATH * 4 ? std::wstring() : std::wstring(buf, n);
    const std::wstring target = StatusLineExe();
    if (self.empty() || target.empty())
    {
        error = L"Can't find the AppData folder.";
        return false;
    }
    if (_wcsicmp(self.c_str(), target.c_str()) == 0 || SameFile(self, target))
        return true;
    const std::wstring dir = target.substr(0, target.rfind(L'\\'));
    const std::wstring temp = target + L".new", old = target + L".old";
    CreateDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    DeleteFileW(old.c_str());
    if (CopyFileW(self.c_str(), temp.c_str(), FALSE) &&
        (!IsFile(target) || MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) &&
        MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    DeleteFileW(temp.c_str());
    error = L"Can't copy LimitsAndTemps.exe to " + dir;
    return false;
}

std::string OurCommand(const std::string& exe, const std::string& chained)
{
    return "\"" + exe + "\"" + (chained.empty() ? std::string(kStatusLineFlag) : kStatusLinePass + chained);
}

// Splits a status-line command of ours into exe and chained command; false for anyone else's.
bool ParseOurCommand(const std::string& command, std::string& exe, std::string& chained)
{
    size_t quote = command.size() > 1 && command[0] == '"' ? command.find('"', 1) : std::string::npos;
    if (quote == std::string::npos)
        return false;
    std::string rest = command.substr(quote + 1);
    const size_t passLen = sizeof(kStatusLinePass) - 1;
    if (rest == kStatusLineFlag)
        chained.clear();
    else if (rest.compare(0, passLen, kStatusLinePass) == 0)
        chained = rest.substr(passLen);
    else
        return false;
    exe = command.substr(1, quote - 1);
    return true;
}

// Claude Code's settings.json; created as an empty object when missing.
struct ClaudeSettings
{
    std::wstring path;
    std::string text;
    Json root;
};

bool LoadSettings(ClaudeSettings& s, std::wstring& error)
{
    s.path = ClaudeDir() + L"\\settings.json";
    if (!IsFile(s.path))
        s.text = "{\n}\n";
    else if (!ReadTextFile(s.path, s.text))
    {
        error = L"Can't read " + s.path;
        return false;
    }
    if (s.text.find_first_not_of(" \t\r\n\xEF\xBB\xBF") == std::string::npos)
        s.text = "{\n}\n";
    if (!ParseJson(s.text, s.root) || s.root.type != Json::Object)
    {
        error = s.path + L" is not plain JSON, so it was left unchanged.";
        return false;
    }
    return true;
}

bool SaveSettings(const ClaudeSettings& s, const std::string& text, std::wstring& error)
{
    if (text == s.text || WriteTextFile(s.path, text)) // Claude Code reloads the file on every write
        return true;
    error = L"Can't write " + s.path;
    return false;
}

bool ReadRateLimits(const Json& limits, AgentUsage& usage)
{
    const long long now = time(NULL);
    struct { const char* key; UsageWindow* window; } map[] = {
        { "five_hour", &usage.fiveHour }, { "seven_day", &usage.week }, { "seven_day_fable", &usage.fableWeek },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    {
        const Json& w = limits[map[i].key];
        double pct = w["used_percentage"].Num();
        double reset = w["resets_at"].Num();
        if (std::isnan(pct))
            continue;
        // Saved before the window rolled over and Claude Code not used since: nothing is used yet.
        if (!std::isnan(reset) && reset < static_cast<double>(now))
            map[i].window->percent = 0, map[i].window->resetsAt = 0;
        else
            map[i].window->percent = pct, map[i].window->resetsAt = std::isnan(reset) ? 0 : static_cast<long long>(reset);
    }
    return !std::isnan(usage.fiveHour.percent) || !std::isnan(usage.week.percent);
}

// Uses Claude Code's own OAuth sign-in for the endpoint behind its /usage screen, unless the status line
// delivered the limits within the last minute. The token is never renewed here: refresh tokens rotate,
// so renewing would sign Claude Code out.
void FetchClaude(AgentUsage& usage)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long stamp = StatusLineStamp();
    const unsigned long long nowStamp = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (stamp != 0 && nowStamp - stamp < kFreshStatusLine && ReadStatusLine(usage))
        return;

    Json creds;
    const Json& oauth = ReadJsonFile(ClaudeDir() + L"\\.credentials.json", creds) ? creds["claudeAiOauth"] : creds;
    const std::string& token = oauth["accessToken"].string;
    double expiresMs = oauth["expiresAt"].Num();
    if (token.empty() || (!std::isnan(expiresMs) && expiresMs / 1000.0 < static_cast<double>(time(NULL))))
    {
        if (!ReadStatusLine(usage))
            usage.error = GetStatusLine() == STATUSLINE_ON ? L"waiting for Claude Code's next reply"
                                                           : L"connect to read its limits";
        return;
    }

    std::wstring headers = L"Authorization: Bearer " + Widen(token) +
                           L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n";
    std::string body;
    DWORD status = HttpGet(L"api.anthropic.com", L"/api/oauth/usage", headers, body);
    if ((status == 401 || status == 403) && ReadStatusLine(usage))
        return;
    if (!CheckStatus(status, kClaude, usage))
        return;
    Json answer;
    if (!JsonParser(body).Parse(answer))
    {
        usage.error = L"unexpected answer";
        return;
    }

    struct { const char* key; UsageWindow* window; } map[] = {
        { "five_hour", &usage.fiveHour }, { "seven_day", &usage.week }, { "seven_day_fable", &usage.fableWeek },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    {
        const Json& w = answer[map[i].key];
        map[i].window->percent = w["utilization"].Num();
        map[i].window->resetsAt = ParseIsoTime(w["resets_at"].string);
    }
    if (std::isnan(usage.fiveHour.percent) && std::isnan(usage.week.percent))
        usage.error = L"unexpected answer";
}

// Uses Codex's ChatGPT sign-in for the endpoint behind its /status limits; never renewed here (see above).
void FetchCodex(AgentUsage& usage)
{
    Json auth;
    if (!ReadJsonFile(CodexDir() + L"\\auth.json", auth))
    {
        usage.error = L"not signed in";
        return;
    }
    const Json& tokens = auth["tokens"];
    const std::string& token = tokens["access_token"].string;
    if (token.empty())
    {
        usage.error = auth["OPENAI_API_KEY"].type == Json::String ? L"API key sign-in has no plan limits" : L"not signed in";
        return;
    }

    std::wstring headers = L"Authorization: Bearer " + Widen(token) + L"\r\nAccept: application/json\r\n";
    if (!tokens["account_id"].string.empty())
        headers += L"ChatGPT-Account-Id: " + Widen(tokens["account_id"].string) + L"\r\n";
    std::string body;
    if (!CheckStatus(HttpGet(L"chatgpt.com", L"/backend-api/wham/usage", headers, body), kCodex, usage))
        return;
    Json answer;
    if (!JsonParser(body).Parse(answer))
    {
        usage.error = L"unexpected answer";
        return;
    }

    // Plans differ in which windows they have; sort them by length rather than by slot.
    const Json& limits = answer["rate_limit"];
    const char* slots[] = { "primary_window", "secondary_window" };
    for (size_t i = 0; i < 2; ++i)
    {
        const Json& w = limits[slots[i]];
        double pct = w["used_percent"].Num();
        if (std::isnan(pct))
            continue;
        double seconds = w["limit_window_seconds"].Num();
        UsageWindow& target = seconds > 24 * 3600 ? usage.week : usage.fiveHour;
        target.percent = pct;
        double reset = w["reset_at"].Num();
        target.resetsAt = std::isnan(reset) ? 0 : static_cast<long long>(reset);
    }
    if (std::isnan(usage.fiveHour.percent) && std::isnan(usage.week.percent))
        usage.error = L"unexpected answer";
}
} // namespace

std::vector<AgentUsage> InstalledAgents()
{
    std::vector<AgentUsage> out;
    if (IsDir(ClaudeDir()))
        out.push_back(Empty(kClaude));
    if (IsDir(CodexDir()))
        out.push_back(Empty(kCodex));
    return out;
}

unsigned long long StatusLineStamp()
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(StatusLinePath().c_str(), GetFileExInfoStandard, &d))
        return 0;
    return (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

bool ReadStatusLine(AgentUsage& claude)
{
    Json saved;
    if (!ReadJsonFile(StatusLinePath(), saved))
        return false;
    UsageWindow none = { kNaN, 0 };
    claude.fiveHour = claude.week = claude.fableWeek = none;
    if (!ReadRateLimits(saved["rate_limits"], claude))
        return false;
    claude.error.clear();
    return true;
}

StatusLineState GetStatusLine()
{
    ClaudeSettings s;
    std::wstring error;
    std::string exe, chained;
    if (!LoadSettings(s, error) || !ParseOurCommand(s.root["statusLine"]["command"].string, exe, chained))
        return STATUSLINE_OFF;
    const std::wstring copy = StatusLineExe();
    return _stricmp(exe.c_str(), CommandPath(copy).c_str()) == 0 && IsFile(copy) ? STATUSLINE_ON : STATUSLINE_STALE;
}

// Edits only the statusLine part of settings.json, so everything else stays byte for byte as it was.
bool ConnectStatusLine(std::wstring& error)
{
    ClaudeSettings s;
    if (!LoadSettings(s, error) || !InstallStatusLineExe(error))
        return false;
    const std::string self = CommandPath(StatusLineExe());
    std::string text = s.text;
    const Json& line = s.root["statusLine"];
    const Json& command = line["command"];
    if (command.type == Json::String)
    {
        // A previous command (or ours from another exe location) keeps running behind ours.
        std::string exe, chained;
        if (!ParseOurCommand(command.string, exe, chained))
            chained = command.string;
        text.replace(command.begin, command.end - command.begin, JsonQuote(OurCommand(self, chained)));
    }
    else
    {
        std::string fresh = "{\n    \"type\": \"command\",\n    \"command\": " + JsonQuote(OurCommand(self, std::string())) + "\n  }";
        if (s.root.Has("statusLine"))
            text.replace(line.begin, line.end - line.begin, fresh);
        else if (s.root.members.empty())
            text.replace(s.root.begin, s.root.end - s.root.begin, "{\n  \"statusLine\": " + fresh + "\n}");
        else
            text.insert(s.root.members.back().second.end, ",\n  \"statusLine\": " + fresh);
    }
    return SaveSettings(s, text, error);
}

bool DisconnectStatusLine(std::wstring& error)
{
    ClaudeSettings s;
    if (!LoadSettings(s, error))
        return false;
    const Json& command = s.root["statusLine"]["command"];
    std::string exe, chained;
    if (!ParseOurCommand(command.string, exe, chained))
        return true; // not ours (any more): nothing to undo
    std::string text = s.text;
    if (!chained.empty())
        text.replace(command.begin, command.end - command.begin, JsonQuote(chained));
    else if (s.root.members.size() == 1)
        text.replace(s.root.begin, s.root.end - s.root.begin, "{}");
    else
    {
        // Remove the whole member together with one neighbouring comma.
        for (size_t i = 0; i < s.root.members.size(); ++i)
        {
            if (s.root.members[i].first != "statusLine")
                continue;
            size_t from = s.root.members[i].second.keyBegin, to = s.root.members[i].second.end;
            size_t after = text.find_first_not_of(" \t\r\n", to);
            if (after != std::string::npos && text[after] == ',')
                to = text.find_first_not_of(" \t\r\n", after + 1);
            else
            {
                size_t before = text.find_last_not_of(" \t\r\n", from - 1);
                if (before != std::string::npos && text[before] == ',')
                    from = before;
            }
            text.erase(from, to - from);
            break;
        }
    }
    if (!SaveSettings(s, text, error))
        return false;
    DeleteFileW(StatusLinePath().c_str()); // would only go stale from now on
    const std::wstring copy = StatusLineExe();
    DeleteFileW(copy.c_str());
    DeleteFileW((copy + L".old").c_str());
    RemoveDirectoryW(copy.substr(0, copy.rfind(L'\\')).c_str()); // only if nothing else is in it
    return true;
}

int RunStatusLine(bool pass)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    std::string input;
    char buf[8192];
    DWORD read = 0;
    while (input.size() < 1024 * 1024 && ReadFile(in, buf, sizeof(buf), &read, NULL) && read > 0)
        input.append(buf, read);
    DWORD written = 0;
    if (pass) // first, so the chained status line isn't delayed
        WriteFile(out, input.data(), static_cast<DWORD>(input.size()), &written, NULL);

    std::string summary;
    Json status;
    const Json& limits = ParseJson(input, status) ? status["rate_limits"] : status;
    AgentUsage claude = Empty(kClaude);
    if (limits.type == Json::Object && ReadRateLimits(limits, claude))
    {
        // Only the limits are kept, nothing about the session. Written aside and renamed, so readers never
        // see half a file.
        const std::wstring path = StatusLinePath(), temp = path + L"." + std::to_wstring(GetCurrentProcessId());
        if (!WriteTextFile(temp, "{\"rate_limits\":" + input.substr(limits.begin, limits.end - limits.begin) + "}\n") ||
            !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(temp.c_str());

        char part[32];
        if (!std::isnan(claude.fiveHour.percent))
        {
            sprintf_s(part, "5h %.0f%%", claude.fiveHour.percent);
            summary += part;
        }
        if (!std::isnan(claude.week.percent))
        {
            sprintf_s(part, "%s1w %.0f%%", summary.empty() ? "" : " \xC2\xB7 ", claude.week.percent);
            summary += part;
        }
    }
    if (!pass)
        WriteFile(out, summary.data(), static_cast<DWORD>(summary.size()), &written, NULL);
    return 0;
}

std::vector<AgentUsage> FetchUsage()
{
    std::vector<AgentUsage> out = InstalledAgents();
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].name == kClaude)
            FetchClaude(out[i]);
        else
            FetchCodex(out[i]);
    }
    return out;
}
