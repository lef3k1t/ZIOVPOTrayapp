#include "ZIOVPOtrayservice.h"
#include "ZIOVPOTrayRpc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <winhttp.h>

namespace
{
const wchar_t kServiceName[] = L"ZIOVPOTrayService";
const wchar_t kRpcEndpoint[] = L"ZIOVPOTrayServiceRpc";
const wchar_t kTrayProcessName[] = L"ZIOVPOtrayapp.exe";
const DWORD kNotAuthenticated = 12001;
const DWORD kNoLicense = 12002;
const DWORD kServerRequestFailed = 12003;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
HANDLE g_refreshEvent = nullptr;
HANDLE g_rpcReadyEvent = nullptr;
CRITICAL_SECTION g_processLock{};
CRITICAL_SECTION g_accountLock{};
CRITICAL_SECTION g_avLock{};
std::vector<PROCESS_INFORMATION> g_trayProcesses;

enum class ObjectType : DWORD
{
    Unknown = 0,
    PeFile = 1,
    PowerShellScript = 2
};

struct AvRecord
{
    ULONGLONG objectSignaturePrefix = 0;
    DWORD objectSignatureLength = 0;
    std::array<BYTE, 32> objectSignature{};
    ULONGLONG offsetBegin = 0;
    ULONGLONG offsetEnd = 0;
    ObjectType objectType = ObjectType::Unknown;
    std::array<BYTE, 32> avRecordSignature{};
    std::wstring detectionName;
};

struct AvDatabaseState
{
    bool loaded = false;
    std::wstring releaseDateUtc;
    std::map<ULONGLONG, std::vector<AvRecord>> records;
};

AvDatabaseState g_avDatabase;

struct AccountState
{
    bool authenticated = false;
    std::wstring userName;
    std::wstring accessToken;
    std::wstring refreshToken;
    ULONGLONG accessExpiresAt = 0;
    ULONGLONG refreshExpiresAt = 0;
    bool licenseActive = false;
    std::wstring licenseTicket;
    std::wstring licenseExpiresAtText;
    ULONGLONG licenseRefreshAt = 0;
};

AccountState g_account;

std::wstring GetModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring fullPath = path;
    const size_t slash = fullPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
}

void WriteDebugLog(const std::wstring& message)
{
    const std::wstring path = GetModuleDirectory() + L"\\TrayAppService-debug.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[2048]{};
    swprintf_s(
        line,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\r\n",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        GetCurrentProcessId(),
        message.c_str());

    DWORD bytes = 0;
    WriteFile(file, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &bytes, nullptr);
    CloseHandle(file);
}

std::wstring GetEnvOrDefault(const wchar_t* name, const wchar_t* fallback)
{
    wchar_t value[1024]{};
    const DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return length > 0 && length < ARRAYSIZE(value) ? value : fallback;
}

std::wstring LoginUrl()
{
    return GetEnvOrDefault(L"ZIOVPO_AUTH_LOGIN_URL", L"https://localhost:8443/auth/login");
}

std::wstring RefreshUrl()
{
    return GetEnvOrDefault(L"ZIOVPO_AUTH_REFRESH_URL", L"https://localhost:8443/auth/refresh");
}

std::wstring LicenseStatusUrl()
{
    return GetEnvOrDefault(L"ZIOVPO_LICENSE_STATUS_URL", L"https://localhost:8443/license/check");
}

std::wstring ActivationUrl()
{
    return GetEnvOrDefault(L"ZIOVPO_LICENSE_ACTIVATE_URL", L"https://localhost:8443/license/activate");
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    if (size > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::wstring Widen(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(size > 0 ? size - 1 : 0, L'\0');
    if (size > 0)
    {
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    }
    return result;
}

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

std::string JsonEscape(const std::wstring& value)
{
    std::string source = Narrow(value);
    std::string result;
    result.reserve(source.size());
    for (char character : source)
    {
        if (character == '\\' || character == '"')
        {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

long long JsonNumber(const std::string& json, const std::string& name);

std::string Base64UrlToJsonPart(std::string value)
{
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0)
    {
        value.push_back('=');
    }

    DWORD decodedSize = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &decodedSize, nullptr, nullptr))
    {
        return {};
    }

    std::string decoded(decodedSize, '\0');
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(decoded.data()), &decodedSize, nullptr, nullptr))
    {
        return {};
    }
    decoded.resize(decodedSize);
    return decoded;
}

ULONGLONG JwtExpiresAtMs(const std::wstring& token, ULONGLONG fallback)
{
    const std::string narrowToken = Narrow(token);
    const size_t firstDot = narrowToken.find('.');
    const size_t secondDot = firstDot == std::string::npos ? std::string::npos : narrowToken.find('.', firstDot + 1);
    if (firstDot == std::string::npos || secondDot == std::string::npos)
    {
        return fallback;
    }

    const std::string payload = Base64UrlToJsonPart(narrowToken.substr(firstDot + 1, secondDot - firstDot - 1));
    const long long expSeconds = JsonNumber(payload, "exp");
    if (expSeconds <= 0)
    {
        return fallback;
    }

    return static_cast<ULONGLONG>(expSeconds) * 1000;
}

std::string JsonString(const std::string& json, const std::string& name)
{
    const std::string key = "\"" + name + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find(':', position + key.size());
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find('"', position + 1);
    if (position == std::string::npos)
    {
        return {};
    }

    std::string result;
    for (++position; position < json.size(); ++position)
    {
        if (json[position] == '"' && json[position - 1] != '\\')
        {
            break;
        }
        result.push_back(json[position]);
    }
    return result;
}

long long JsonNumber(const std::string& json, const std::string& name)
{
    const std::string key = "\"" + name + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos)
    {
        return 0;
    }

    position = json.find(':', position + key.size());
    if (position == std::string::npos)
    {
        return 0;
    }

    while (++position < json.size() && isspace(static_cast<unsigned char>(json[position])))
    {
    }

    return _strtoi64(json.c_str() + position, nullptr, 10);
}

bool JsonBool(const std::string& json, const std::string& name)
{
    const std::string key = "\"" + name + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos)
    {
        return false;
    }

    position = json.find(':', position + key.size());
    return position != std::string::npos && json.find("true", position) != std::string::npos;
}

ULONGLONG NowMs()
{
    return GetTickCount64();
}

ULONGLONG SecondsFromNow(long long seconds)
{
    return NowMs() + static_cast<ULONGLONG>(std::max<long long>(seconds, 60)) * 1000;
}

std::wstring GetDeviceName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = ARRAYSIZE(name);
    return GetComputerNameW(name, &size) ? std::wstring(name, size) : L"WindowsDevice";
}

std::wstring GetDeviceMac()
{
    ULONG bufferSize = 16 * 1024;
    std::vector<BYTE> buffer(bufferSize);
    auto addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addresses, &bufferSize) == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(bufferSize);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }

    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addresses, &bufferSize) != NO_ERROR)
    {
        return L"00-00-00-00-00-00";
    }

    for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter; adapter = adapter->Next)
    {
        if (adapter->PhysicalAddressLength == 0 || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        wchar_t mac[32]{};
        swprintf_s(
            mac,
            ARRAYSIZE(mac),
            L"%02X-%02X-%02X-%02X-%02X-%02X",
            adapter->PhysicalAddress[0],
            adapter->PhysicalAddress[1],
            adapter->PhysicalAddress[2],
            adapter->PhysicalAddress[3],
            adapter->PhysicalAddress[4],
            adapter->PhysicalAddress[5]);
        return mac;
    }

    return L"00-00-00-00-00-00";
}

std::string LicenseCheckBody()
{
    return "{\"deviceMac\":\"" + JsonEscape(GetDeviceMac()) + "\",\"productId\":1}";
}

std::string LicenseActivationBody(const wchar_t* activationCode)
{
    return "{\"activationKey\":\"" + JsonEscape(activationCode ? activationCode : L"") +
        "\",\"deviceMac\":\"" + JsonEscape(GetDeviceMac()) +
        "\",\"deviceName\":\"" + JsonEscape(GetDeviceName()) + "\"}";
}

std::array<BYTE, 32> Sha256(const std::vector<BYTE>& bytes)
{
    std::array<BYTE, 32> result{};
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;

    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, bytes.data(), static_cast<DWORD>(bytes.size()), 0))
    {
        DWORD hashSize = static_cast<DWORD>(result.size());
        CryptGetHashParam(hash, HP_HASHVAL, result.data(), &hashSize, 0);
    }

    if (hash)
    {
        CryptDestroyHash(hash);
    }
    if (provider)
    {
        CryptReleaseContext(provider, 0);
    }

    return result;
}

ULONGLONG PrefixFromBytes(const BYTE* bytes)
{
    ULONGLONG prefix = 0;
    memcpy(&prefix, bytes, sizeof(prefix));
    return prefix;
}

std::vector<BYTE> BytesFromAscii(const char* text)
{
    const size_t length = strlen(text);
    return std::vector<BYTE>(reinterpret_cast<const BYTE*>(text), reinterpret_cast<const BYTE*>(text) + length);
}

AvRecord MakeAvRecord(const char* signature, ULONGLONG offsetBegin, ULONGLONG offsetEnd, ObjectType objectType, const wchar_t* detectionName)
{
    std::vector<BYTE> bytes = BytesFromAscii(signature);
    AvRecord record{};
    record.objectSignaturePrefix = PrefixFromBytes(bytes.data());
    record.objectSignatureLength = static_cast<DWORD>(bytes.size());
    record.objectSignature = Sha256(bytes);
    record.offsetBegin = offsetBegin;
    record.offsetEnd = offsetEnd;
    record.objectType = objectType;
    record.detectionName = detectionName;

    std::vector<BYTE> recordBytes;
    recordBytes.insert(recordBytes.end(), reinterpret_cast<BYTE*>(&record.objectSignaturePrefix), reinterpret_cast<BYTE*>(&record.objectSignaturePrefix) + sizeof(record.objectSignaturePrefix));
    recordBytes.insert(recordBytes.end(), reinterpret_cast<BYTE*>(&record.objectSignatureLength), reinterpret_cast<BYTE*>(&record.objectSignatureLength) + sizeof(record.objectSignatureLength));
    recordBytes.insert(recordBytes.end(), record.objectSignature.begin(), record.objectSignature.end());
    recordBytes.insert(recordBytes.end(), reinterpret_cast<BYTE*>(&record.offsetBegin), reinterpret_cast<BYTE*>(&record.offsetBegin) + sizeof(record.offsetBegin));
    recordBytes.insert(recordBytes.end(), reinterpret_cast<BYTE*>(&record.offsetEnd), reinterpret_cast<BYTE*>(&record.offsetEnd) + sizeof(record.offsetEnd));
    const DWORD type = static_cast<DWORD>(record.objectType);
    recordBytes.insert(recordBytes.end(), reinterpret_cast<const BYTE*>(&type), reinterpret_cast<const BYTE*>(&type) + sizeof(type));
    record.avRecordSignature = Sha256(recordBytes);
    return record;
}

void AddAvRecordLocked(const AvRecord& record)
{
    g_avDatabase.records[record.objectSignaturePrefix].push_back(record);
}

void LoadAvDatabases()
{
    EnterCriticalSection(&g_avLock);
    g_avDatabase.records.clear();
    AddAvRecordLocked(MakeAvRecord("EICAR-PE-SAMPLE", 0, 1024 * 1024, ObjectType::PeFile, L"Test.PE.EicarLike"));
    AddAvRecordLocked(MakeAvRecord("EICAR-PS-SAMPLE", 0, 1024 * 1024, ObjectType::PowerShellScript, L"Test.PowerShell.EicarLike"));

    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t releaseDate[64]{};
    swprintf_s(
        releaseDate,
        L"%04u-%02u-%02uT%02u:%02u:%02uZ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    g_avDatabase.releaseDateUtc = releaseDate;
    g_avDatabase.loaded = true;
    WriteDebugLog(L"AV databases loaded, records=" + std::to_wstring(g_avDatabase.records.size()));
    LeaveCriticalSection(&g_avLock);
}

size_t AvRecordCountLocked()
{
    size_t count = 0;
    for (const auto& item : g_avDatabase.records)
    {
        count += item.second.size();
    }
    return count;
}

std::wstring ExtensionFromPath(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    return dot == std::wstring::npos ? L"" : ToLower(path.substr(dot));
}

ObjectType DetectObjectType(const std::wstring& path, const std::vector<BYTE>& bytes)
{
    if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z')
    {
        return ObjectType::PeFile;
    }

    const std::wstring extension = ExtensionFromPath(path);
    if (extension == L".ps1" || extension == L".psm1" || extension == L".psd1")
    {
        return ObjectType::PowerShellScript;
    }

    return ObjectType::Unknown;
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0)
    {
        bytes.clear();
        return true;
    }

    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file.good() || file.eof();
}

bool ScanByteStream(const std::vector<BYTE>& bytes, ObjectType objectType, std::wstring& detectionName)
{
    if (bytes.size() < 8)
    {
        return false;
    }

    EnterCriticalSection(&g_avLock);
    for (size_t position = 0; position + 8 <= bytes.size(); ++position)
    {
        const ULONGLONG prefix = PrefixFromBytes(bytes.data() + position);
        const auto found = g_avDatabase.records.find(prefix);
        if (found == g_avDatabase.records.end())
        {
            continue;
        }

        for (const AvRecord& record : found->second)
        {
            if (record.objectType != objectType)
            {
                continue;
            }

            if (position < record.offsetBegin || position > record.offsetEnd)
            {
                continue;
            }

            if (record.objectSignatureLength < 8 || position + record.objectSignatureLength > bytes.size())
            {
                continue;
            }

            std::vector<BYTE> candidate(bytes.begin() + position, bytes.begin() + position + record.objectSignatureLength);
            if (Sha256(candidate) == record.objectSignature)
            {
                detectionName = record.detectionName;
                LeaveCriticalSection(&g_avLock);
                return true;
            }
        }
    }

    LeaveCriticalSection(&g_avLock);
    return false;
}

bool ScanSingleFile(const std::wstring& path, std::wstring& result)
{
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, bytes))
    {
        result = L"Ошибка чтения файла: " + path;
        return false;
    }

    const ObjectType objectType = DetectObjectType(path, bytes);
    std::wstring detectionName;
    if (ScanByteStream(bytes, objectType, detectionName))
    {
        result = L"Обнаружено: " + detectionName + L" в " + path;
        return true;
    }

    result = L"Угроз не обнаружено: " + path;
    return false;
}

void ScanDirectoryRecursive(const std::wstring& directory, DWORD& scannedFiles, DWORD& infectedFiles, std::wstring& firstDetection)
{
    std::wstring search = directory;
    if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
    {
        search += L"\\";
    }

    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW((search + L"*").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        const std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        const std::wstring fullPath = search + name;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            ScanDirectoryRecursive(fullPath, scannedFiles, infectedFiles, firstDetection);
        }
        else
        {
            ++scannedFiles;
            std::wstring fileResult;
            if (ScanSingleFile(fullPath, fileResult))
            {
                ++infectedFiles;
                if (firstDetection.empty())
                {
                    firstDetection = fileResult;
                }
            }
        }
    } while (FindNextFileW(find, &findData));

    FindClose(find);
}

struct HttpResponse
{
    DWORD statusCode = 0;
    std::string body;
};

bool SendHttpsJson(const std::wstring& url, const wchar_t* method, const std::string& body, const std::wstring& bearerToken, HttpResponse& response)
{
    WriteDebugLog(L"HTTP request: " + std::wstring(method) + L" " + url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
    {
        return false;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"ZIOVPOTrayService/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && host == L"localhost")
    {
        DWORD securityFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!bearerToken.empty())
    {
        headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
    }

    const BOOL sent = request && WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    bool ok = false;
    if (sent && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
        response.statusCode = statusCode;

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read))
            {
                break;
            }
            chunk.resize(read);
            response.body += chunk;
        }

        ok = statusCode >= 200 && statusCode < 300;
    }

    if (request)
    {
        WinHttpCloseHandle(request);
    }
    if (connection)
    {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    WriteDebugLog(L"HTTP response status=" + std::to_wstring(response.statusCode));
    return ok;
}

void CopyRpcString(wchar_t* destination, unsigned long destinationChars, const std::wstring& value)
{
    if (!destination || destinationChars == 0)
    {
        return;
    }

    wcsncpy_s(destination, destinationChars, value.c_str(), _TRUNCATE);
}

bool UpdateLicenseStatusLocked()
{
    if (!g_account.authenticated || g_account.accessToken.empty())
    {
        g_account.licenseActive = false;
        g_account.licenseTicket.clear();
        return false;
    }

    HttpResponse response{};
    if (!SendHttpsJson(LicenseStatusUrl(), L"POST", LicenseCheckBody(), g_account.accessToken, response))
    {
        return false;
    }

    g_account.licenseTicket = Widen(response.body);
    g_account.licenseActive = !JsonBool(response.body, "licenseBlocked") && !JsonString(response.body, "licenseEndingDate").empty();
    g_account.licenseExpiresAtText = Widen(JsonString(response.body, "licenseEndingDate"));
    if (g_account.licenseExpiresAtText.empty())
    {
        g_account.licenseExpiresAtText = Widen(JsonString(response.body, "expiresAt"));
    }
    if (g_account.licenseExpiresAtText.empty())
    {
        g_account.licenseExpiresAtText = Widen(JsonString(response.body, "expires_at"));
    }

    const long long refreshInSeconds = JsonNumber(response.body, "ticketTtlSeconds");
    const long long expiresInSeconds = JsonNumber(response.body, "expiresIn");
    g_account.licenseRefreshAt = SecondsFromNow(refreshInSeconds > 0 ? refreshInSeconds : std::max<long long>(expiresInSeconds / 2, 300));
    return g_account.licenseActive;
}

bool RefreshTokensLocked()
{
    if (!g_account.authenticated || g_account.refreshToken.empty())
    {
        return false;
    }

    const std::string body = "{\"refreshToken\":\"" + JsonEscape(g_account.refreshToken) + "\"}";
    HttpResponse response{};
    if (!SendHttpsJson(RefreshUrl(), L"POST", body, {}, response))
    {
        return false;
    }

    const std::wstring accessToken = Widen(JsonString(response.body, "accessToken"));
    const std::wstring refreshToken = Widen(JsonString(response.body, "refreshToken"));
    if (!accessToken.empty())
    {
        g_account.accessToken = accessToken;
    }
    if (!refreshToken.empty())
    {
        g_account.refreshToken = refreshToken;
    }

    const long long accessExpiresIn = JsonNumber(response.body, "accessExpiresIn");
    const long long refreshExpiresIn = JsonNumber(response.body, "refreshExpiresIn");
    g_account.accessExpiresAt = JwtExpiresAtMs(g_account.accessToken, SecondsFromNow(accessExpiresIn > 0 ? accessExpiresIn : 900));
    g_account.refreshExpiresAt = JwtExpiresAtMs(g_account.refreshToken, SecondsFromNow(refreshExpiresIn > 0 ? refreshExpiresIn : 86400));
    return true;
}

void ClearAccountLocked()
{
    g_account = AccountState{};
    SetEvent(g_refreshEvent);
}

void EnablePrivilege(const wchar_t* privilegeName)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, privilegeName, &privileges.Privileges[0].Luid))
    {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    }

    CloseHandle(token);
}

void EnableProcessCreationPrivileges()
{
    EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnablePrivilege(SE_INCREASE_QUOTA_NAME);
    EnablePrivilege(SE_TCB_NAME);
}

void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32ExitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_SESSIONCHANGE : 0;

    if (g_statusHandle)
    {
        SetServiceStatus(g_statusHandle, &g_status);
    }
    WriteDebugLog(L"SetServiceState state=" + std::to_wstring(state) + L", error=" + std::to_wstring(win32ExitCode));
}

std::wstring GetServiceDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring fullPath = path;
    const size_t slash = fullPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
}

bool IsProcessRunning(const PROCESS_INFORMATION& processInfo)
{
    return WaitForSingleObject(processInfo.hProcess, 0) == WAIT_TIMEOUT;
}

bool HasTrayInSession(DWORD sessionId)
{
    EnterCriticalSection(&g_processLock);
    const bool exists = std::any_of(g_trayProcesses.begin(), g_trayProcesses.end(), [sessionId](const PROCESS_INFORMATION& processInfo) {
        DWORD processSessionId = 0;
        ProcessIdToSessionId(processInfo.dwProcessId, &processSessionId);
        return processSessionId == sessionId && IsProcessRunning(processInfo);
    });
    LeaveCriticalSection(&g_processLock);
    return exists;
}

void CleanupExitedTrayProcesses()
{
    EnterCriticalSection(&g_processLock);
    auto iterator = g_trayProcesses.begin();
    while (iterator != g_trayProcesses.end())
    {
        if (!IsProcessRunning(*iterator))
        {
            CloseHandle(iterator->hProcess);
            CloseHandle(iterator->hThread);
            iterator = g_trayProcesses.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    LeaveCriticalSection(&g_processLock);
}

void StartTrayForSession(DWORD sessionId)
{
    WriteDebugLog(L"StartTrayForSession sessionId=" + std::to_wstring(sessionId));
    if (sessionId == 0 || HasTrayInSession(sessionId))
    {
        WriteDebugLog(L"StartTrayForSession skipped");
        return;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken))
    {
        WriteDebugLog(L"WTSQueryUserToken failed: " + std::to_wstring(GetLastError()));
        return;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken))
    {
        WriteDebugLog(L"DuplicateTokenEx failed: " + std::to_wstring(GetLastError()));
        CloseHandle(userToken);
        return;
    }

    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, primaryToken, FALSE);

    const std::wstring serviceDirectory = GetServiceDirectory();
    const std::wstring trayPath = serviceDirectory + L"\\" + kTrayProcessName;
    std::wstring commandLine = L"\"" + trayPath + L"\" --hidden --service-child";
    WriteDebugLog(L"CreateProcessAsUserW path=" + trayPath);

    STARTUPINFO startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessAsUserW(
        primaryToken,
        trayPath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        environment,
        serviceDirectory.c_str(),
        &startupInfo,
        &processInfo);

    if (environment)
    {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primaryToken);
    CloseHandle(userToken);

    if (created)
    {
        WriteDebugLog(L"CreateProcessAsUserW success pid=" + std::to_wstring(processInfo.dwProcessId));
        EnterCriticalSection(&g_processLock);
        g_trayProcesses.push_back(processInfo);
        LeaveCriticalSection(&g_processLock);
    }
    else
    {
        WriteDebugLog(L"CreateProcessAsUserW failed: " + std::to_wstring(GetLastError()));
    }
}

void StartTrayForActiveSessions()
{
    WriteDebugLog(L"StartTrayForActiveSessions");
    PWTS_SESSION_INFO sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount))
    {
        WriteDebugLog(L"WTSEnumerateSessionsW failed: " + std::to_wstring(GetLastError()));
        return;
    }

    for (DWORD index = 0; index < sessionCount; ++index)
    {
        if (sessions[index].SessionId != 0 && (sessions[index].State == WTSActive || sessions[index].State == WTSConnected))
        {
            StartTrayForSession(sessions[index].SessionId);
        }
    }

    WTSFreeMemory(sessions);

    const DWORD consoleSessionId = WTSGetActiveConsoleSessionId();
    WriteDebugLog(L"Active console session=" + std::to_wstring(consoleSessionId));
    if (consoleSessionId != 0 && consoleSessionId != 0xFFFFFFFF)
    {
        StartTrayForSession(consoleSessionId);
    }
}

void StopAllTrayProcesses()
{
    EnterCriticalSection(&g_processLock);
    for (const PROCESS_INFORMATION& processInfo : g_trayProcesses)
    {
        if (IsProcessRunning(processInfo))
        {
            TerminateProcess(processInfo.hProcess, 0);
        }
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    g_trayProcesses.clear();
    LeaveCriticalSection(&g_processLock);
}

DWORD WINAPI SessionMonitorThread(void*)
{
    while (WaitForSingleObject(g_stopEvent, 5000) == WAIT_TIMEOUT)
    {
        CleanupExitedTrayProcesses();
        StartTrayForActiveSessions();
    }
    return 0;
}

DWORD WINAPI AccountRefreshThread(void*)
{
    while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT)
    {
        EnterCriticalSection(&g_accountLock);
        const bool authenticated = g_account.authenticated;
        const bool hasLicenseTicket = !g_account.licenseTicket.empty();
        const ULONGLONG now = NowMs();
        const bool refreshToken = authenticated && g_account.accessExpiresAt > 0 && now + 60000 >= g_account.accessExpiresAt;
        const bool refreshLicense = authenticated && hasLicenseTicket && g_account.licenseRefreshAt > 0 && now >= g_account.licenseRefreshAt;

        if (refreshToken)
        {
            RefreshTokensLocked();
        }
        if (refreshLicense)
        {
            UpdateLicenseStatusLocked();
        }
        LeaveCriticalSection(&g_accountLock);

        WaitForSingleObject(g_refreshEvent, 30000);
        ResetEvent(g_refreshEvent);
    }
    return 0;
}

DWORD WINAPI RpcServerThread(void*)
{
    WriteDebugLog(L"RPC server thread starting");
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr);

    if (status != RPC_S_OK && status != RPC_S_DUPLICATE_ENDPOINT)
    {
        WriteDebugLog(L"RpcServerUseProtseqEpW failed: " + std::to_wstring(status));
        SetEvent(g_stopEvent);
        return status;
    }

    status = RpcServerRegisterIf(ZIOVPOTrayRpc_v1_0_s_ifspec, nullptr, nullptr);
    if (status != RPC_S_OK)
    {
        WriteDebugLog(L"RpcServerRegisterIf failed: " + std::to_wstring(status));
        SetEvent(g_stopEvent);
        return status;
    }

    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK && status != RPC_S_ALREADY_LISTENING && status != RPC_S_SERVER_TOO_BUSY)
    {
        WriteDebugLog(L"RpcServerListen failed: " + std::to_wstring(status));
        SetEvent(g_stopEvent);
        return status;
    }

    WriteDebugLog(L"RPC server is listening");
    SetEvent(g_rpcReadyEvent);
    RpcMgmtWaitServerListen();
    RpcServerUnregisterIf(ZIOVPOTrayRpc_v1_0_s_ifspec, nullptr, FALSE);
    WriteDebugLog(L"RPC server stopped listening");
    SetEvent(g_stopEvent);
    return 0;
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD eventType, void* eventData, void*)
{
    if (control == SERVICE_CONTROL_SESSIONCHANGE)
    {
        if (eventType == WTS_SESSION_LOGON || eventType == WTS_SESSION_UNLOCK || eventType == WTS_CONSOLE_CONNECT || eventType == WTS_REMOTE_CONNECT)
        {
            const auto notification = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (notification)
            {
                StartTrayForSession(notification->dwSessionId);
            }
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    WriteDebugLog(L"ServiceMain entered");
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandler, nullptr);
    if (!g_statusHandle)
    {
        return;
    }

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);
    EnableProcessCreationPrivileges();
    InitializeCriticalSection(&g_processLock);
    InitializeCriticalSection(&g_accountLock);
    InitializeCriticalSection(&g_avLock);
    LoadAvDatabases();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_refreshEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_rpcReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent || !g_refreshEvent || !g_rpcReadyEvent)
    {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        DeleteCriticalSection(&g_processLock);
        DeleteCriticalSection(&g_accountLock);
        DeleteCriticalSection(&g_avLock);
        return;
    }

    HANDLE rpcThread = CreateThread(nullptr, 0, RpcServerThread, nullptr, 0, nullptr);
    if (!rpcThread)
    {
        WriteDebugLog(L"CreateThread(RPC) failed: " + std::to_wstring(GetLastError()));
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    WaitForSingleObject(g_rpcReadyEvent, 10000);
    SetServiceState(SERVICE_RUNNING);

    HANDLE monitorThread = CreateThread(nullptr, 0, SessionMonitorThread, nullptr, 0, nullptr);
    HANDLE accountThread = CreateThread(nullptr, 0, AccountRefreshThread, nullptr, 0, nullptr);

    StartTrayForActiveSessions();

    WaitForSingleObject(g_stopEvent, INFINITE);
    SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);

    RpcMgmtStopServerListening(nullptr);
    if (rpcThread)
    {
        WaitForSingleObject(rpcThread, 5000);
        CloseHandle(rpcThread);
    }

    if (monitorThread)
    {
        WaitForSingleObject(monitorThread, 5000);
        CloseHandle(monitorThread);
    }

    if (accountThread)
    {
        WaitForSingleObject(accountThread, 5000);
        CloseHandle(accountThread);
    }

    StopAllTrayProcesses();
    CloseHandle(g_rpcReadyEvent);
    CloseHandle(g_refreshEvent);
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_accountLock);
    DeleteCriticalSection(&g_processLock);
    DeleteCriticalSection(&g_avLock);
    SetServiceState(SERVICE_STOPPED);
}
}

void RpcStopService(handle_t)
{
    RpcMgmtStopServerListening(nullptr);
}

error_status_t RpcGetCurrentUser(handle_t, int* authenticated, wchar_t* userName, unsigned long userNameChars)
{
    EnterCriticalSection(&g_accountLock);
    *authenticated = g_account.authenticated ? 1 : 0;
    CopyRpcString(userName, userNameChars, g_account.authenticated ? g_account.userName : L"");
    LeaveCriticalSection(&g_accountLock);
    return RPC_S_OK;
}

error_status_t RpcLogin(handle_t, const wchar_t* userName, const wchar_t* password, int* authenticated)
{
    *authenticated = 0;
    const std::string body = "{\"email\":\"" + JsonEscape(userName ? userName : L"") + "\",\"password\":\"" + JsonEscape(password ? password : L"") + "\"}";
    HttpResponse response{};
    if (!SendHttpsJson(LoginUrl(), L"POST", body, {}, response))
    {
        return kServerRequestFailed;
    }

    const std::wstring accessToken = Widen(JsonString(response.body, "accessToken"));
    const std::wstring refreshToken = Widen(JsonString(response.body, "refreshToken"));
    if (accessToken.empty() || refreshToken.empty())
    {
        return kNotAuthenticated;
    }

    EnterCriticalSection(&g_accountLock);
    g_account.authenticated = true;
    g_account.userName = userName ? userName : L"";
    g_account.accessToken = accessToken;
    g_account.refreshToken = refreshToken;
    g_account.accessExpiresAt = JwtExpiresAtMs(g_account.accessToken, SecondsFromNow(JsonNumber(response.body, "accessExpiresIn") > 0 ? JsonNumber(response.body, "accessExpiresIn") : 900));
    g_account.refreshExpiresAt = JwtExpiresAtMs(g_account.refreshToken, SecondsFromNow(JsonNumber(response.body, "refreshExpiresIn") > 0 ? JsonNumber(response.body, "refreshExpiresIn") : 86400));
    g_account.licenseActive = false;
    g_account.licenseTicket.clear();
    UpdateLicenseStatusLocked();
    LeaveCriticalSection(&g_accountLock);

    SetEvent(g_refreshEvent);
    *authenticated = 1;
    return RPC_S_OK;
}

void RpcLogout(handle_t)
{
    EnterCriticalSection(&g_accountLock);
    ClearAccountLocked();
    LeaveCriticalSection(&g_accountLock);
}

error_status_t RpcGetLicenseInfo(handle_t, int* active, wchar_t* expiresAtUtc, unsigned long expiresAtChars)
{
    EnterCriticalSection(&g_accountLock);
    if (!g_account.authenticated)
    {
        *active = 0;
        CopyRpcString(expiresAtUtc, expiresAtChars, L"");
        LeaveCriticalSection(&g_accountLock);
        return kNotAuthenticated;
    }

    if (g_account.licenseTicket.empty())
    {
        UpdateLicenseStatusLocked();
    }

    *active = g_account.licenseActive ? 1 : 0;
    CopyRpcString(expiresAtUtc, expiresAtChars, g_account.licenseExpiresAtText);
    const error_status_t result = g_account.licenseActive ? RPC_S_OK : kNoLicense;
    LeaveCriticalSection(&g_accountLock);
    return result;
}

error_status_t RpcActivateProduct(handle_t, const wchar_t* activationCode, int* active, wchar_t* expiresAtUtc, unsigned long expiresAtChars)
{
    *active = 0;
    EnterCriticalSection(&g_accountLock);
    if (!g_account.authenticated)
    {
        CopyRpcString(expiresAtUtc, expiresAtChars, L"");
        LeaveCriticalSection(&g_accountLock);
        return kNotAuthenticated;
    }

    const std::string body = LicenseActivationBody(activationCode);
    HttpResponse response{};
    if (!SendHttpsJson(ActivationUrl(), L"POST", body, g_account.accessToken, response))
    {
        LeaveCriticalSection(&g_accountLock);
        return kServerRequestFailed;
    }

    if (!response.body.empty() && (!JsonString(response.body, "licenseEndingDate").empty() || !JsonString(response.body, "signature").empty()))
    {
        g_account.licenseTicket = Widen(response.body);
        g_account.licenseActive = !JsonBool(response.body, "licenseBlocked") && !JsonString(response.body, "licenseEndingDate").empty();
        g_account.licenseExpiresAtText = Widen(JsonString(response.body, "licenseEndingDate"));
        if (g_account.licenseExpiresAtText.empty())
        {
            g_account.licenseExpiresAtText = Widen(JsonString(response.body, "expiresAt"));
        }
        if (g_account.licenseExpiresAtText.empty())
        {
            g_account.licenseExpiresAtText = Widen(JsonString(response.body, "expires_at"));
        }
        g_account.licenseRefreshAt = SecondsFromNow(std::max<long long>(JsonNumber(response.body, "ticketTtlSeconds"), 300));
    }
    else
    {
        UpdateLicenseStatusLocked();
    }

    *active = g_account.licenseActive ? 1 : 0;
    CopyRpcString(expiresAtUtc, expiresAtChars, g_account.licenseExpiresAtText);
    const error_status_t result = g_account.licenseActive ? RPC_S_OK : kNoLicense;
    if (g_account.licenseActive)
    {
        LoadAvDatabases();
    }
    LeaveCriticalSection(&g_accountLock);
    return result;
}

error_status_t RpcGetAvDatabaseInfo(handle_t, unsigned long* recordCount, wchar_t* releaseDateUtc, unsigned long releaseDateChars)
{
    EnterCriticalSection(&g_avLock);
    *recordCount = static_cast<unsigned long>(AvRecordCountLocked());
    CopyRpcString(releaseDateUtc, releaseDateChars, g_avDatabase.releaseDateUtc);
    LeaveCriticalSection(&g_avLock);
    return RPC_S_OK;
}

error_status_t RpcScanFile(handle_t, const wchar_t* path, int* infected, wchar_t* result, unsigned long resultChars)
{
    *infected = 0;
    EnterCriticalSection(&g_accountLock);
    const bool hasLicense = g_account.licenseActive && !g_account.licenseTicket.empty();
    LeaveCriticalSection(&g_accountLock);
    if (!hasLicense)
    {
        CopyRpcString(result, resultChars, L"Сканирование заблокировано: нет активной лицензии");
        return kNoLicense;
    }

    std::wstring scanResult;
    const bool detected = ScanSingleFile(path ? path : L"", scanResult);
    *infected = detected ? 1 : 0;
    CopyRpcString(result, resultChars, scanResult);
    return RPC_S_OK;
}

error_status_t RpcScanDirectory(handle_t, const wchar_t* path, unsigned long* scannedFiles, unsigned long* infectedFiles, wchar_t* result, unsigned long resultChars)
{
    *scannedFiles = 0;
    *infectedFiles = 0;
    EnterCriticalSection(&g_accountLock);
    const bool hasLicense = g_account.licenseActive && !g_account.licenseTicket.empty();
    LeaveCriticalSection(&g_accountLock);
    if (!hasLicense)
    {
        CopyRpcString(result, resultChars, L"Сканирование заблокировано: нет активной лицензии");
        return kNoLicense;
    }

    DWORD scanned = 0;
    DWORD infected = 0;
    std::wstring firstDetection;
    ScanDirectoryRecursive(path ? path : L"", scanned, infected, firstDetection);

    *scannedFiles = scanned;
    *infectedFiles = infected;
    std::wstring summary = L"Проверено файлов: " + std::to_wstring(scanned) + L". Обнаружено угроз: " + std::to_wstring(infected) + L".";
    if (!firstDetection.empty())
    {
        summary += L"\r\n" + firstDetection;
    }
    CopyRpcString(result, resultChars, summary);
    return RPC_S_OK;
}

void* __RPC_USER midl_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER midl_user_free(void* pointer)
{
    free(pointer);
}

int wmain()
{
    SERVICE_TABLE_ENTRY serviceTable[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        return GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? 0 : 1;
    }

    return 0;
}
