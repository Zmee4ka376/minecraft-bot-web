#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <fstream>
#include <map>
#include <atomic>

#ifdef _WIN32
// Console UTF-8 support without pulling in <windows.h>: it #defines THIS
// (COM helper macro) which breaks ProtocolCraft's "using THIS = TDerived"
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#endif

#include "botcraft/Utilities/Logger.hpp"
#include "botcraft/Game/Entities/EntityManager.hpp"
#include "botcraft/Game/Entities/LocalPlayer.hpp"
#include "botcraft/Game/Inventory/InventoryManager.hpp"
#include "botcraft/Game/Inventory/Window.hpp"
#include "botcraft/Game/AssetsManager.hpp"
#include "WebBotClient.hpp"

#include <asio.hpp>

#ifdef _WIN32
// Crash dump support. windows.h is included AFTER all project headers on
// purpose: it #defines THIS (COM helper macro) which breaks ProtocolCraft's
// "using THIS = TDerived" if it comes first.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* ep)
{
    CreateDirectoryA("dumps", nullptr);
    char name[128];
    __time64_t t = 0;
    time(&t);
    tm lt{};
    localtime_s(&lt, &t);
    snprintf(name, sizeof(name), "dumps/crash_%04d%02d%02d_%02d%02d%02d.dmp",
        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    HANDLE file = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        HMODULE dbg = LoadLibraryA("dbghelp.dll");
        if (dbg != nullptr)
        {
            using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD,
                PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
            const MiniDumpWriteDump_t write_dump =
                reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbg, "MiniDumpWriteDump"));
            if (write_dump != nullptr)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData,
                    &mei, nullptr, nullptr);
            }
        }
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static WebBotClient* g_client = nullptr;
static std::atomic<bool> g_running(true);

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {}
            else { out += c; }
            break;
        }
    }
    return out;
}

static std::string UrlDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            const auto hex = [](const char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int h1 = hex(s[i + 1]);
            const int h2 = hex(s[i + 2]);
            if (h1 >= 0 && h2 >= 0)
            {
                out += static_cast<char>((h1 << 4) | h2);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') { out += ' '; }
        else { out += s[i]; }
    }
    return out;
}

static std::string MakeResponse(const std::string& status, const std::string& content_type, const std::string& body)
{
    std::ostringstream os;
    os << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Cache-Control: no-store\r\n"
        << "\r\n"
        << body;
    return os.str();
}

static std::string FormField(const std::string& body, const std::string& key)
{
    const std::string prefix = key + "=";
    const size_t pos = body.find(prefix);
    if (pos == std::string::npos) { return ""; }
    const size_t start = pos + prefix.size();
    const size_t end = body.find('&', start);
    return UrlDecode(body.substr(start, end == std::string::npos ? body.size() - start : end - start));
}

static int FormInt(const std::string& body, const std::string& key, const int def)
{
    const std::string v = FormField(body, key);
    if (v.empty()) { return def; }
    try { return std::stoi(v); }
    catch (const std::exception&) { return def; }
}

static std::string ParseJsonField(const std::string& body, const std::string& field)
{
    std::string key = "\"" + field + "\"";
    size_t pos = body.find(key);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < body.size() && body[pos] == ' ') pos++;
    if (pos >= body.size()) return "";
    if (body[pos] == '"') {
        pos++;
        size_t end = body.find('"', pos);
        if (end == std::string::npos) return "";
        return body.substr(pos, end - pos);
    }
    size_t end = pos;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ']') end++;
    return body.substr(pos, end - pos);
}

static std::string BuildInventoryJson()
{
    if (!g_client) return "{\"error\":\"no client\"}";

    const auto selected = g_client->GetStatus().hotbar_index;
    const auto hotbar = g_client->GetHotbar();
    const auto inventory = g_client->GetInventory();

    auto slot_name = [](const WebBotClient::SlotData& s) -> std::string {
        return s.present ? s.name : "";
    };
    auto slot_count = [](const WebBotClient::SlotData& s) -> int {
        return s.present ? s.count : 0;
    };

    std::ostringstream os;
    os << "{";
    os << "\"selected\":" << selected << ",";

    WebBotClient::SlotData handSlot{};
    for (const auto& s : hotbar)
    {
        if (s.index == selected) { handSlot = s; break; }
    }
    os << "\"hand\":{\"name\":\"" << JsonEscape(slot_name(handSlot)) << "\",\"count\":" << slot_count(handSlot) << ",\"empty\":" << (handSlot.present ? "false" : "true") << "},";

    WebBotClient::SlotData offhand{};
    for (const auto& s : inventory)
    {
        if (s.index == 45) { offhand = s; break; }
    }
    os << "\"offhand\":{\"name\":\"" << JsonEscape(slot_name(offhand)) << "\",\"count\":" << slot_count(offhand) << ",\"empty\":" << (offhand.present ? "false" : "true") << "},";

    std::map<std::string, int> totals;
    for (const auto& s : inventory)
    {
        if (s.present && s.index >= 9 && s.index <= 44)
        {
            totals[s.name] += s.count;
        }
    }
    os << "\"items\":[";
    bool first_item = true;
    for (const auto& [name, count] : totals)
    {
        if (!first_item) os << ",";
        first_item = false;
        os << "{\"name\":\"" << JsonEscape(name) << "\",\"count\":" << count << "}";
    }
    os << "]";

    os << ",\"slots\":[";
    bool first_slot = true;
    for (const auto& s : inventory)
    {
        if (s.index < 9 || s.index > 44) continue;
        if (!first_slot) os << ",";
        first_slot = false;
        os << "{\"slot\":" << s.index
            << ",\"name\":\"" << JsonEscape(slot_name(s)) << "\""
            << ",\"count\":" << slot_count(s)
            << ",\"empty\":" << (s.present ? "false" : "true")
            << "}";
    }
    os << "]";
    os << "}";
    return os.str();
}

static std::string BuildContainerJson()
{
    if (!g_client) return "{\"error\":\"no client\"}";

    auto ci = g_client->GetContainerInfo();
    if (!ci.is_open) return "{\"open\":false}";

    std::ostringstream os;
    os << "{";
    os << "\"open\":true,";
    os << "\"window\":" << ci.window_id << ",";
    os << "\"columns\":9,";
    os << "\"slots\":[";
    bool first = true;
    for (const auto& s : ci.slots)
    {
        if (!first) os << ",";
        first = false;
        os << "{\"slot\":" << s.index << ","
            << "\"name\":\"" << JsonEscape(s.name) << "\","
            << "\"count\":" << s.count << ","
            << "\"empty\":" << (s.present ? "false" : "true") << "}";
    }
    os << "]}";
    return os.str();
}

static std::string IndexHtml();

static void HandleClient(asio::ip::tcp::socket socket)
{
    try
    {
        std::string raw;
        {
            char tmp[4096];
            asio::error_code ec;
            while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 65536)
            {
                const size_t n = socket.read_some(asio::buffer(tmp, sizeof(tmp)), ec);
                if (ec) break;
                raw.append(tmp, n);
            }
        }

        const size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) { socket.close(); return; }

        const std::string header = raw.substr(0, header_end);
        std::string body = raw.substr(header_end + 4);

        std::string method, path;
        const size_t sp1 = header.find(' ');
        const size_t sp2 = header.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos)
        {
            method = header.substr(0, sp1);
            path = header.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        size_t content_length = 0;
        const size_t cl = header.find("Content-Length:");
        if (cl != std::string::npos)
        {
            size_t v = cl + 15;
            while (v < header.size() && header[v] == ' ') ++v;
            const size_t e = header.find("\r\n", v);
            try { content_length = std::stoul(header.substr(v, e - v)); }
            catch (...) { content_length = 0; }
        }
        if (content_length == 0)
        {
            const size_t cl2 = header.find("content-length:");
            if (cl2 != std::string::npos)
            {
                size_t v = cl2 + 15;
                while (v < header.size() && header[v] == ' ') ++v;
                const size_t e = header.find("\r\n", v);
                try { content_length = std::stoul(header.substr(v, e - v)); }
                catch (...) { content_length = 0; }
            }
        }

        while (body.size() < content_length)
        {
            char tmp[4096];
            asio::error_code ec;
            const size_t n = socket.read_some(asio::buffer(tmp, sizeof(tmp)), ec);
            if (ec) break;
            body.append(tmp, n);
        }
        if (body.size() > content_length) body.resize(content_length);

        std::string response;

        if (method == "OPTIONS")
        {
            response = MakeResponse("200 OK", "text/plain", "");
        }
        else if (method == "GET" && (path == "/" || path == "/index.html"))
        {
            response = MakeResponse("200 OK", "text/html; charset=utf-8", IndexHtml());
        }
        else if (method == "GET" && path == "/app.js")
        {
            std::ifstream f("static/app.js", std::ios::binary);
            if (!f.is_open()) f.open("../static/app.js", std::ios::binary);
            if (!f.is_open()) f.open("bin/static/app.js", std::ios::binary);
            std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            response = MakeResponse("200 OK", "application/javascript; charset=utf-8", js);
        }
        else if (method == "GET" && path == "/style.css")
        {
            std::ifstream f("static/style.css", std::ios::binary);
            if (!f.is_open()) f.open("../static/style.css", std::ios::binary);
            if (!f.is_open()) f.open("bin/static/style.css", std::ios::binary);
            std::string css((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            response = MakeResponse("200 OK", "text/css; charset=utf-8", css);
        }
        else if (method == "GET" && path == "/api/status")
        {
            std::ostringstream os;
            os << "{\"name\":\"" << JsonEscape(g_client ? g_client->GetPlayerName() : "") << "\",\"connected\":"
               << (g_client && !g_client->GetPlayerName().empty() ? "true" : "false") << "}";
            response = MakeResponse("200 OK", "application/json; charset=utf-8", os.str());
        }
        else if (method == "GET" && path.rfind("/api/messages", 0) == 0)
        {
            long long since = -1;
            const size_t q = path.find("since=");
            if (q != std::string::npos)
            {
                try { since = std::stoll(path.substr(q + 6)); }
                catch (...) { since = -1; }
            }
            const auto messages = g_client ? g_client->GetLogSince(since) : std::vector<std::pair<long long, std::string>>();
            std::ostringstream os;
            os << "{\"messages\":[";
            for (size_t i = 0; i < messages.size(); ++i)
            {
                if (i > 0) os << ",";
                os << "[" << messages[i].first << ",\"" << JsonEscape(messages[i].second) << "\"]";
            }
            os << "]}";
            response = MakeResponse("200 OK", "application/json; charset=utf-8", os.str());
        }
        else if (method == "POST" && path == "/api/send")
        {
            const size_t eq = body.find('=');
            std::string message = eq == std::string::npos ? std::string() : UrlDecode(body.substr(eq + 1));
            if (g_client && !message.empty())
            {
                g_client->SendChatMessage(message);
                g_client->LogMessage("<" + g_client->GetPlayerName() + "> " + message);
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/command")
        {
            const size_t eq = body.find('=');
            std::string command = eq == std::string::npos ? std::string() : UrlDecode(body.substr(eq + 1));
            if (g_client && !command.empty())
            {
                g_client->SendChatMessage("/" + command);
                g_client->LogMessage("[/" + command + "]");
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/botcmd")
        {
            const size_t eq = body.find('=');
            std::string command = eq == std::string::npos ? std::string() : UrlDecode(body.substr(eq + 1));
            if (g_client && !command.empty())
            {
                g_client->ProcessLocalCommand(command);
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "GET" && path == "/api/inventory")
        {
            response = MakeResponse("200 OK", "application/json; charset=utf-8", BuildInventoryJson());
        }
        else if (method == "GET" && path == "/api/container")
        {
            response = MakeResponse("200 OK", "application/json; charset=utf-8", BuildContainerJson());
        }
        else if (method == "POST" && path == "/api/hotbar")
        {
            if (g_client)
            {
                g_client->SelectHotbarSlot(FormInt(body, "slot", 0));
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/click")
        {
            if (g_client)
            {
                g_client->ClickSlot(
                    static_cast<short>(FormInt(body, "window", 0)),
                    static_cast<short>(FormInt(body, "slot", 0)),
                    FormInt(body, "type", 0),
                    static_cast<char>(FormInt(body, "button", 0)));
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/swap")
        {
            if (g_client)
            {
                g_client->SwapSlots(
                    static_cast<short>(FormInt(body, "window", 0)),
                    static_cast<short>(FormInt(body, "a", 0)),
                    static_cast<short>(FormInt(body, "b", 0)));
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/drop")
        {
            if (g_client)
            {
                g_client->DropSlot(
                    static_cast<short>(FormInt(body, "window", 0)),
                    static_cast<short>(FormInt(body, "slot", 0)));
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/close")
        {
            if (g_client)
            {
                g_client->CloseWindow(static_cast<short>(FormInt(body, "window", -1)));
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/handin")
        {
            const std::string item = FormField(body, "item");
            if (g_client && !item.empty())
            {
                g_client->SetItemInHandByName(item);
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else if (method == "POST" && path == "/api/use")
        {
            if (g_client)
            {
                g_client->UseItem();
            }
            response = MakeResponse("200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        }
        else
        {
            response = MakeResponse("404 Not Found", "text/plain", "Not Found");
        }

        asio::write(socket, asio::buffer(response));
        socket.close();
    }
    catch (...) {}
}

static void RunHttpServer(int port)
{
    try
    {
        asio::io_context io;
        asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));

        LOG_INFO("Web panel listening on http://127.0.0.1:" << port);

        while (g_running)
        {
            asio::ip::tcp::socket socket(io);
            asio::error_code ec;
            acceptor.accept(socket, ec);
            if (ec || !g_running) break;

            std::thread([sock = std::move(socket)]() mutable {
                HandleClient(std::move(sock));
            }).detach();
        }
    }
    catch (std::exception& e)
    {
        LOG_ERROR("HTTP server error: " << e.what());
    }
}

void ShowHelp(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  -h, --help           Show help\n"
        << "  --address ADDR       Server address (default: 127.0.0.1:25565)\n"
        << "  --login NAME         Bot name (default: WebBot)\n"
        << "  --web-port PORT      Web interface port (default: 8080)\n"
        << std::endl;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(WriteCrashDump);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try
    {
        Botcraft::Logger::GetInstance().SetLogLevel(Botcraft::LogLevel::Info);
        Botcraft::Logger::GetInstance().SetFilename("logs/bot.log");
        Botcraft::Logger::GetInstance().RegisterThread("main");

        std::string address = "127.0.0.1:25565";
        std::string login = "WebBot";
        int web_port = 8080;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") { ShowHelp(argv[0]); return 0; }
            else if (arg == "--address" && i + 1 < argc) { address = argv[++i]; }
            else if (arg == "--login" && i + 1 < argc) { login = argv[++i]; }
            else if (arg == "--web-port" && i + 1 < argc) { web_port = std::stoi(argv[++i]); }
        }

        std::thread http_thread(RunHttpServer, web_port);

        bool want_running = true;
        while (want_running)
        {
            WebBotClient client;
            client.SetAutoRespawn(true);
            g_client = &client;

            try
            {
                LOG_INFO("Connecting to " << address << " as " << login);
                client.Connect(address, login);
                LOG_INFO("Connected! Bot name: " << client.GetPlayerName());

                client.RunBehaviourUntilClosed();

                client.Disconnect();
            }
            catch (std::exception& e)
            {
                LOG_ERROR("Connection error: " << e.what());
                try { client.Disconnect(); } catch (...) {}
            }

            g_client = nullptr;
            LOG_INFO("Disconnected. Reconnecting in 5s...");

            for (int i = 0; i < 50 && want_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        g_running = false;
        http_thread.join();

        return 0;
    }
    catch (std::exception& e)
    {
        LOG_FATAL("Exception: " << e.what());
        return 1;
    }
}

static std::string IndexHtml()
{
    static const std::string html = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Botcraft Control Panel</title>
<style>
  body { font-family: Consolas, monospace; background: #1e1e1e; color: #d4d4d4; margin: 0; padding: 16px; }
  h1 { font-size: 18px; margin: 0 0 8px; color: #4ec9b0; }
  h2 { font-size: 14px; margin: 12px 0 6px; color: #9cdcfe; }
  #status { color: #9cdcfe; margin-bottom: 12px; }
  .row { display: flex; gap: 8px; margin-bottom: 8px; }
  input { flex: 1; background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 8px; border-radius: 4px; font-family: Consolas, monospace; }
  select { flex: 1; background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 8px; border-radius: 4px; font-family: Consolas, monospace; }
  button { background: #0e639c; color: white; border: none; padding: 8px 14px; border-radius: 4px; cursor: pointer; font-family: Consolas, monospace; }
  button:hover { background: #1177bb; }
  button.danger { background: #a1260d; }
  button.small { padding: 3px 7px; font-size: 11px; }
  .grid { display: grid; gap: 3px; margin-bottom: 6px; }
  .grid.hotbar { grid-template-columns: repeat(9, 1fr); }
  .grid.main { grid-template-columns: repeat(9, 1fr); }
  .grid.win { grid-template-columns: repeat(9, 1fr); }
  .slot { position: relative; background: #252525; border: 1px solid #3a3a3a; border-radius: 4px; padding: 4px; min-height: 40px; cursor: pointer; }
  .slot.sel { border: 2px solid #4ec9b0; }
  .slot-name { font-size: 10px; color: #ce9178; word-break: break-word; }
  .slot-count { font-size: 11px; color: #b5cea8; }
  .slot .drop { position: absolute; top: 1px; right: 1px; background: #5a1d1d; padding: 0 4px; font-size: 10px; display: none; cursor: pointer; }
  .slot:hover .drop { display: block; }
  .hb { position: relative; background: #252525; border: 1px solid #3a3a3a; border-radius: 4px; padding: 4px; min-height: 40px; cursor: pointer; text-align: center; }
  .hb.cur { border: 2px solid #d7ba7d; }
  .hb .num { color: #569cd6; font-size: 10px; }
  #log { background: #111; border: 1px solid #333; border-radius: 4px; height: 300px; overflow-y: auto; padding: 8px; font-size: 13px; }
  #log div { white-space: pre-wrap; word-break: break-word; padding: 1px 0; }
  .hint { color: #808080; font-size: 11px; margin: 4px 0 8px; }
  input:focus { border-color: #4ec9b0; outline: none; }
</style>
</head>
<body>
  <h1>Botcraft Control Panel</h1>
  <div id="status">status: ...</div>
  <div class="row">
    <input id="chat" placeholder="Сообщение в чат..." />
    <button onclick="sendChat()">Отправить</button>
  </div>
  <div class="row">
    <input id="command" placeholder="Команда (без /)..." />
    <button onclick="sendCommand()">Команда</button>
  </div>
  <div class="row">
    <input id="botcmd" placeholder="Команда боту: goto 100 64 -200 / dig x y z / interact x y z / stop..." />
    <button onclick="sendBotCmd()">Боту</button>
  </div>
  <div class="hint">Команды боту: goto x y z · dig x y z · interact x y z · stop</div>

  <h2>Инвентарь</h2>
  <div id="handinfo" class="hint">В руке: —</div>
  <div class="row">
    <select id="itemselect"></select>
    <button onclick="takeInHand()">Взять в руку</button>
    <button onclick="useItem()">Использовать</button>
  </div>
  <div id="invlist" class="hint"></div>
  <div class="hint">Клик по слоту — выбрать, клик по второму — переложить. ✕ при наведении — выбросить стак.</div>
  <div class="grid main" id="invgrid"></div>

  <h2>Хотбар (клик — выбрать слот)</h2>
  <div class="grid hotbar" id="hotbar"></div>

  <h2>Сундук (клик — взять, Shift+клик — быстро переложить)</h2>
  <div id="containerinfo" class="hint">Сундук не открыт</div>
  <div class="row">
    <button class="small" onclick="closeContainer()">Закрыть сундук</button>
  </div>
  <div class="grid win" id="container"></div>

  <h2>Лог</h2>
  <div id="log"></div>
<script>
let lastId = -1;
const logEl = document.getElementById('log');

async function pollMessages() {
  try {
    const r = await fetch('/api/messages?since=' + lastId);
    const j = await r.json();
    for (const m of j.messages) {
      const div = document.createElement('div');
      div.textContent = m[1];
      logEl.appendChild(div);
      lastId = m[0];
    }
    while (logEl.children.length > 500) logEl.removeChild(logEl.firstChild);
    logEl.scrollTop = logEl.scrollHeight;
  } catch (e) {}
}

async function pollStatus() {
  try {
    const r = await fetch('/api/status');
    const j = await r.json();
    document.getElementById('status').textContent =
      'бот: ' + j.name + (j.connected ? ' (подключён)' : ' (не подключён)');
  } catch (e) {}
}

async function post(path, bodyObj) {
  await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(bodyObj).toString()
  });
}

function renderInventory(j) {
  document.getElementById('handinfo').textContent =
    'В руке: ' + (j.hand.empty ? '—' : (j.hand.name + (j.hand.count > 1 ? ' x' + j.hand.count : '')))
    + '   |   Левая рука: ' + (j.offhand.empty ? '—' : j.offhand.name);

  const sel = document.getElementById('itemselect');
  const prev = sel.value;
  sel.innerHTML = '';
  const empty = document.createElement('option');
  empty.value = '';
  empty.textContent = '— выберите предмет —';
  sel.appendChild(empty);
  j.items.forEach(it => {
    const o = document.createElement('option');
    o.value = it.name;
    o.textContent = it.name + ' (x' + it.count + ')';
    sel.appendChild(o);
  });
  sel.value = prev;

  document.getElementById('invlist').textContent =
    j.items.length === 0 ? 'Инвентарь пуст' : 'Предметы: ' + j.items.map(it => it.name + ' x' + it.count).join(', ');

  const hb = document.getElementById('hotbar');
  hb.innerHTML = '';
  for (let p = 0; p < 9; ++p) {
    const d = document.createElement('div');
    d.className = 'hb' + (j.selected === p ? ' cur' : '');
    const num = document.createElement('div');
    num.className = 'num';
    num.textContent = String(p + 1);
    d.appendChild(num);
    d.addEventListener('click', () => post('/api/hotbar', { slot: p }));
    hb.appendChild(d);
  }

  renderInvGrid(j);
}

let selInvSlot = -1;
let lastInvData = null;

function renderInvGrid(j) {
  lastInvData = j;
  const grid = document.getElementById('invgrid');
  grid.innerHTML = '';
  if (!j.slots) return;
  for (const s of j.slots) {
    const d = document.createElement('div');
    d.className = 'slot' + (selInvSlot === s.slot ? ' sel' : '');
    d.title = s.empty ? '' : s.name;
    if (!s.empty) {
      const name = document.createElement('div');
      name.className = 'slot-name';
      name.textContent = s.name.replace('minecraft:', '');
      d.appendChild(name);
      if (s.count > 1) {
        const cnt = document.createElement('div');
        cnt.className = 'slot-count';
        cnt.textContent = 'x' + s.count;
        d.appendChild(cnt);
      }
      const drop = document.createElement('div');
      drop.className = 'drop';
      drop.textContent = '\u2715';
      drop.title = 'Выбросить';
      drop.addEventListener('click', (e) => {
        e.stopPropagation();
        post('/api/drop', { window: 0, slot: s.slot });
      });
      d.appendChild(drop);
    }
    d.addEventListener('click', () => {
      if (selInvSlot === -1) {
        selInvSlot = s.slot;
        renderInvGrid(j);
      } else if (selInvSlot === s.slot) {
        selInvSlot = -1;
        renderInvGrid(j);
      } else {
        post('/api/swap', { window: 0, a: selInvSlot, b: s.slot });
        selInvSlot = -1;
      }
    });
    grid.appendChild(d);
  }
}

async function pollInventory() {
  try {
    const r = await fetch('/api/inventory');
    const j = await r.json();
    if (!j.error) renderInventory(j);
  } catch (e) {}
}

function renderContainer(j) {
  const info = document.getElementById('containerinfo');
  const grid = document.getElementById('container');
  if (!j || j.error || !j.open) {
    info.textContent = 'Сундук не открыт';
    grid.innerHTML = '';
    return;
  }
  info.textContent = 'Сундук открыт (окно ' + j.window + ')';
  grid.style.gridTemplateColumns = 'repeat(' + j.columns + ', 1fr)';
  grid.innerHTML = '';
  for (const s of j.slots) {
    const d = document.createElement('div');
    d.className = 'slot';
    if (!s.empty) {
      const name = document.createElement('div');
      name.className = 'slot-name';
      name.textContent = s.name;
      d.appendChild(name);
      if (s.count > 1) {
        const cnt = document.createElement('div');
        cnt.className = 'slot-count';
        cnt.textContent = 'x' + s.count;
        d.appendChild(cnt);
      }
    }
    d.addEventListener('click', (e) => {
      const type = e.shiftKey ? 1 : 0;
      post('/api/click', { window: j.window, slot: s.slot, type: type, button: 0 });
    });
    grid.appendChild(d);
  }
}

async function pollContainer() {
  try {
    const r = await fetch('/api/container');
    const j = await r.json();
    if (!j.error) renderContainer(j);
  } catch (e) {}
}

function takeInHand() {
  const v = document.getElementById('itemselect').value;
  if (v) post('/api/handin', { item: v });
}
function useItem() { post('/api/use', {}); }
function closeContainer() { post('/api/close', { window: -1 }); }

async function sendChat() {
  const v = document.getElementById('chat').value;
  if (v) { await post('/api/send', { message: v }); document.getElementById('chat').value = ''; }
}
async function sendCommand() {
  const v = document.getElementById('command').value;
  if (v) { await post('/api/command', { command: v }); document.getElementById('command').value = ''; }
}
async function sendBotCmd() {
  const v = document.getElementById('botcmd').value;
  if (v) { await post('/api/botcmd', { command: v }); document.getElementById('botcmd').value = ''; }
}

document.getElementById('chat').addEventListener('keydown', e => { if (e.key === 'Enter') sendChat(); });
document.getElementById('command').addEventListener('keydown', e => { if (e.key === 'Enter') sendCommand(); });
document.getElementById('botcmd').addEventListener('keydown', e => { if (e.key === 'Enter') sendBotCmd(); });

pollStatus();
setInterval(pollStatus, 3000);
setInterval(pollMessages, 500);
pollInventory();
setInterval(pollInventory, 1500);
pollContainer();
setInterval(pollContainer, 1000);
</script>
</body>
</html>)HTML";
    return html;
}
