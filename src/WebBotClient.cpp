#include <sstream>
#include <iterator>
#include <chrono>
#include <algorithm>

#include "protocolCraft/Utilities/Json.hpp"

#include "botcraft/Game/World/World.hpp"
#include "botcraft/Game/Entities/EntityManager.hpp"
#include "botcraft/Game/Entities/LocalPlayer.hpp"
#include "botcraft/Game/Inventory/InventoryManager.hpp"
#include "botcraft/Game/Inventory/Window.hpp"
#include "botcraft/Game/Inventory/Item.hpp"
#include "botcraft/Game/AssetsManager.hpp"
#include "botcraft/Network/NetworkManager.hpp"
#include "botcraft/AI/BehaviourTree.hpp"
#include "botcraft/AI/Tasks/AllTasks.hpp"

#include "WebBotClient.hpp"
#include "MineModule.hpp"

using namespace Botcraft;
using namespace ProtocolCraft;

WebBotClient::WebBotClient() : TemplatedBehaviourClient<WebBotClient>(), log_counter(0)
{
}

WebBotClient::~WebBotClient()
{
}

void WebBotClient::LogMessage(const std::string& line)
{
    const long long id = log_counter.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        log.push_back({ id, line });
        if (log.size() > 500)
        {
            log.pop_front();
        }
    }
}

std::vector<std::pair<long long, std::string>> WebBotClient::GetLogSince(const long long last_id) const
{
    std::vector<std::pair<long long, std::string>> result;
    std::lock_guard<std::mutex> lock(log_mutex);
    for (const auto& m : log)
    {
        if (m.first > last_id)
        {
            result.push_back(m);
        }
    }
    return result;
}

long long WebBotClient::GetLogLastId() const
{
    return log_counter.load() - 1;
}

std::string WebBotClient::GetPlayerName()
{
    if (network_manager)
        return network_manager->GetMyName();
    return "";
}

void WebBotClient::SelectHotbarSlot(const int slot)
{
    if (slot < 0 || slot > 8 || !network_manager)
    {
        return;
    }
    auto packet = std::make_shared<ServerboundSetCarriedItemPacket>();
    packet->SetSlot(static_cast<short>(slot));
    network_manager->Send(packet);
    if (inventory_manager)
    {
        inventory_manager->SetIndexHotbarSelected(static_cast<short>(slot));
    }
}

bool WebBotClient::ClickSlot(const short window_id, const short slot, const int click_type, const char button)
{
    return ClickSlotInContainer(*this, window_id, slot, click_type, button) == Status::Success;
}

bool WebBotClient::SwapSlots(const short window_id, const short first_slot, const short second_slot)
{
    return SwapItemsInContainer(*this, window_id, first_slot, second_slot) == Status::Success;
}

bool WebBotClient::DropSlot(const short window_id, const short slot)
{
    return DropItemsFromContainer(*this, window_id, slot, 0) == Status::Success;
}

bool WebBotClient::CloseWindow(const short window_id)
{
    return CloseContainer(*this, window_id) == Status::Success;
}

bool WebBotClient::SetItemInHandByName(const std::string& item_name)
{
    return SetItemInHand(*this, item_name, Hand::Right) == Status::Success;
}

void WebBotClient::UseItem()
{
    if (!network_manager)
    {
        return;
    }
    auto packet = std::make_shared<ServerboundUseItemPacket>();
    packet->SetHand(static_cast<int>(Hand::Right));
    network_manager->Send(packet);
}

void WebBotClient::ProcessLocalCommand(const std::string& line)
{
    std::istringstream ss{ line };
    const std::vector<std::string> splitted({ std::istream_iterator<std::string>{ss}, std::istream_iterator<std::string>{} });
    if (splitted.empty() || !network_manager)
    {
        return;
    }

    LogMessage("[CMD] " + line);

    std::vector<std::string> full;
    full.reserve(splitted.size() + 1);
    full.push_back(network_manager->GetMyName());
    full.insert(full.end(), splitted.begin(), splitted.end());
    ProcessChatMsg(full);
}

void WebBotClient::ExecuteCommand(const std::string& cmd)
{
    std::vector<std::string> splitted;
    splitted.push_back(GetPlayerName());
    std::istringstream ss(cmd);
    std::string word;
    while (ss >> word)
    {
        splitted.push_back(word);
    }
    ProcessChatMsg(splitted);
}

void WebBotClient::AddLocalChatMessage(const std::string& sender, const std::string& text)
{
    std::lock_guard<std::mutex> lock(chatMutex);
    ChatMsg cm;
    cm.sender = sender;
    cm.text = text;
    cm.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chatHistory.push_back(cm);
    if (chatHistory.size() > MAX_CHAT_HISTORY)
    {
        chatHistory.erase(chatHistory.begin());
    }
}

#if PROTOCOL_VERSION < 759
void WebBotClient::Handle(ClientboundChatPacket& msg)
{
    ManagersClient::Handle(msg);

    std::string text = msg.GetMessage().GetText();
    std::string sender;

    try
    {
        auto root = ProtocolCraft::Json::Parse(msg.GetMessage().GetRawText());
        if (root.is_object() && root.contains("translate") && root["translate"].is_string()
            && root["translate"].get_string() == "chat.type.text"
            && root.contains("with") && root["with"].is_array() && root["with"].size() > 0)
        {
            const auto& name_component = root["with"][0u];
            if (name_component.is_object() && name_component.contains("text") && name_component["text"].is_string())
            {
                sender = name_component["text"].get_string();
            }
        }
    }
    catch (...) {}

    if (sender.empty())
    {
        std::vector<std::string> splitted;
        std::istringstream ss(text);
        splitted.assign(std::istream_iterator<std::string>(ss), std::istream_iterator<std::string>());
        sender = splitted.empty() ? "" : splitted[0];
    }

    LogMessage("<" + sender + "> " + text);

    {
        std::lock_guard<std::mutex> lock(chatMutex);
        ChatMsg cm;
        cm.sender = sender;
        cm.text = text;
        cm.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chatHistory.push_back(cm);
        if (chatHistory.size() > MAX_CHAT_HISTORY) {
            chatHistory.erase(chatHistory.begin());
        }
    }

    std::vector<std::string> splitted;
    splitted.push_back(sender);
    std::istringstream ss(text);
    std::string word;
    while (ss >> word)
    {
        splitted.push_back(word);
    }
    ProcessChatMsg(splitted);
}
#else
void WebBotClient::Handle(ClientboundPlayerChatPacket& msg)
{
    ManagersClient::Handle(msg);
}
void WebBotClient::Handle(ClientboundSystemChatPacket& msg)
{
    ManagersClient::Handle(msg);
}
#endif

void WebBotClient::Handle(ClientboundRespawnPacket& msg)
{
    ManagersClient::Handle(msg);
    // On a proxy network a Respawn is often just a server transfer
    // (hub <-> game server): let the mining module start its anti-cheat
    // grace period. Remove together with the MineModule.
    MineModule::NotifyTransfer();
}

void WebBotClient::ProcessChatMsg(const std::vector<std::string>& splitted_msg)
{
    if (splitted_msg.size() < 2)
    {
        return;
    }

    std::string sender = splitted_msg[0];
    if (sender.size() >= 2 && sender.front() == '<' && sender.back() == '>')
    {
        sender = sender.substr(1, sender.size() - 2);
    }

    if (sender != network_manager->GetMyName())
    {
        return;
    }

    // Optional mining module (mine/sortdebris/seallava/place_block/check_perimeter/use).
    // Remove this call (and the CMakeLists entries) to build without the module.
    if (MineModule::ProcessCommand(*this, splitted_msg))
    {
        return;
    }

    if (splitted_msg[1] == "goto")
    {
        if (splitted_msg.size() < 5)
        {
            SendChatMessage("Usage: [BotName] goto x y z [speed]");
            return;
        }
        Position target_position;
        float speed_multiplier = 1.0f;
        try
        {
            target_position = Position(std::stoi(splitted_msg[2]), std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4]));
            if (splitted_msg.size() > 5)
                speed_multiplier = std::stof(splitted_msg[5]);
        }
        catch (...) { return; }

        auto tree = Builder<WebBotClient>("goto")
            .sequence()
                .selector()
                    .leaf("go", [=](WebBotClient& c) { return GoTo(c, target_position, 0, 0, 0, true, false, speed_multiplier); })
                    .leaf(Say, "Pathfinding failed :(")
                .end()
                .leaf([](WebBotClient& c) { c.SetBehaviourTree(nullptr); return Status::Success; })
            .end();
        SetBehaviourTree(tree);
    }
    else if (splitted_msg[1] == "stop")
    {
        SetBehaviourTree(nullptr);
    }
    else if (splitted_msg[1] == "interact")
    {
        if (splitted_msg.size() < 5)
        {
            SendChatMessage("Usage: [BotName] interact x y z");
            return;
        }
        Position pos;
        try { pos = Position(std::stoi(splitted_msg[2]), std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4])); }
        catch (...) { return; }

        auto tree = Builder<WebBotClient>("interact")
            .sequence()
                .succeeder().sequence()
                    .leaf("go next", GoTo, pos, 4, 0, 1, true, false, 1.0f)
                    .leaf(SetBlackboardData<Position>, "InteractWithBlock.pos", pos)
                    .leaf("interact", InteractWithBlockBlackboard)
                    .leaf(RemoveBlackboardData, "InteractWithBlock.pos")
                .end()
                .leaf([](WebBotClient& c) { c.SetBehaviourTree(nullptr); return Status::Success; })
            .end();
        SetBehaviourTree(tree);
    }
    else if (splitted_msg[1] == "dig")
    {
        if (splitted_msg.size() < 5)
        {
            SendChatMessage("Usage: [BotName] dig x y z");
            return;
        }
        Position pos;
        try { pos = Position(std::stoi(splitted_msg[2]), std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4])); }
        catch (...) { return; }

        auto tree = Builder<WebBotClient>("dig")
            .sequence()
                .succeeder().leaf("dig", Dig, pos, true, PlayerDiggingFace::Up, true)
                .leaf([](WebBotClient& c) { c.SetBehaviourTree(nullptr); return Status::Success; })
            .end();
        SetBehaviourTree(tree);
    }
    else if (splitted_msg[1] == "die")
    {
        should_be_closed = true;
    }
}

std::vector<WebBotClient::ChatMsg> WebBotClient::GetChatHistory()
{
    std::lock_guard<std::mutex> lock(chatMutex);
    return chatHistory;
}

WebBotClient::BotStatus WebBotClient::GetStatus()
{
    BotStatus s{};
    auto player = entity_manager->GetLocalPlayer();
    if (player)
    {
        s.x = player->GetX();
        s.y = player->GetY();
        s.z = player->GetZ();
        s.yaw = player->GetYaw();
        s.pitch = player->GetPitch();
        s.health = player->GetHealth();
        s.food = player->GetFood();
        s.saturation = player->GetFoodSaturation();
        s.on_ground = player->GetOnGround();

        switch (player->GetGameMode()) {
            case GameType::Survival: s.gamemode = "survival"; break;
            case GameType::Creative: s.gamemode = "creative"; break;
            case GameType::Adventure: s.gamemode = "adventure"; break;
            case GameType::Spectator: s.gamemode = "spectator"; break;
            default: s.gamemode = "unknown"; break;
        }
    }
    s.hotbar_index = inventory_manager->GetIndexHotbarSelected();
    return s;
}

static std::string ItemNameById(const int id)
{
    if (id <= 0) return "";
    const auto& items = AssetsManager::getInstance().Items();
    const auto it = items.find(id);
    return it == items.end() ? ("id:" + std::to_string(id)) : it->second->GetName();
}

std::vector<WebBotClient::SlotData> WebBotClient::GetInventory()
{
    std::vector<SlotData> result;
    auto inv = inventory_manager->GetPlayerInventory();
    if (!inv) return result;

    auto slots = inv->GetSlots();
    for (auto& [idx, slot] : slots)
    {
        SlotData sd;
        sd.index = idx;
        sd.present = !slot.IsEmptySlot();
        sd.item_id = sd.present ? slot.GetItemId() : -1;
        sd.count = sd.present ? slot.GetItemCount() : 0;
        sd.name = sd.present ? ItemNameById(sd.item_id) : "";
        result.push_back(sd);
    }
    return result;
}

std::vector<WebBotClient::SlotData> WebBotClient::GetHotbar()
{
    std::vector<SlotData> result;
    auto inv = inventory_manager->GetPlayerInventory();
    if (!inv) return result;

    for (short i = Window::INVENTORY_HOTBAR_START; i <= Window::INVENTORY_HOTBAR_START + 8; ++i)
    {
        Slot slot = inv->GetSlot(i);
        SlotData sd;
        sd.index = i - Window::INVENTORY_HOTBAR_START;
        sd.present = !slot.IsEmptySlot();
        sd.item_id = sd.present ? slot.GetItemId() : -1;
        sd.count = sd.present ? slot.GetItemCount() : 0;
        sd.name = sd.present ? ItemNameById(sd.item_id) : "";
        result.push_back(sd);
    }
    return result;
}

WebBotClient::ContainerInfo WebBotClient::GetContainerInfo()
{
    ContainerInfo ci{};
    ci.is_open = false;
    ci.window_id = -1;

    short wid = inventory_manager->GetFirstOpenedWindowId();
    if (wid <= 0) return ci;

    auto window = inventory_manager->GetWindow(wid);
    if (!window) return ci;

    ci.is_open = true;
    ci.window_id = wid;

    auto slots_map = window->GetSlots();
    for (const auto& [idx, slot] : slots_map)
    {
        SlotData sd;
        sd.index = idx;
        sd.present = !slot.IsEmptySlot();
        sd.item_id = sd.present ? slot.GetItemId() : -1;
        sd.count = sd.present ? slot.GetItemCount() : 0;
        sd.name = sd.present ? ItemNameById(sd.item_id) : "";
        ci.slots.push_back(sd);
    }
    return ci;
}
