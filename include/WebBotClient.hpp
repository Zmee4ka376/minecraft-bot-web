#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "botcraft/Game/Vector3.hpp"
#include "botcraft/AI/TemplatedBehaviourClient.hpp"

class WebBotClient : public Botcraft::TemplatedBehaviourClient<WebBotClient>
{
public:
    WebBotClient();
    ~WebBotClient();

    struct ChatMsg {
        std::string sender;
        std::string text;
        long long timestamp;
    };

    struct BotStatus {
        double x, y, z;
        float yaw, pitch;
        float health;
        int food;
        float saturation;
        bool on_ground;
        std::string gamemode;
        std::string dimension;
        short hotbar_index;
    };

    struct SlotData {
        short index;
        int item_id;
        int count;
        std::string name;
        bool present;
    };

    struct ContainerInfo {
        bool is_open;
        short window_id;
        std::string title;
        int type;
        int size;
        std::vector<SlotData> slots;
    };

    std::vector<ChatMsg> GetChatHistory();
    BotStatus GetStatus();
    std::vector<SlotData> GetInventory();
    std::vector<SlotData> GetHotbar();
    ContainerInfo GetContainerInfo();
    std::string GetPlayerName();

    void LogMessage(const std::string& line);
    std::vector<std::pair<long long, std::string>> GetLogSince(const long long last_id) const;
    long long GetLogLastId() const;

    void SelectHotbarSlot(const int slot);
    bool ClickSlot(const short window_id, const short slot, const int click_type, const char button);
    bool SwapSlots(const short window_id, const short first_slot, const short second_slot);
    bool DropSlot(const short window_id, const short slot);
    bool CloseWindow(const short window_id);
    bool SetItemInHandByName(const std::string& item_name);
    void UseItem();
    void ProcessLocalCommand(const std::string& line);
    void ExecuteCommand(const std::string& cmd);
    void AddLocalChatMessage(const std::string& sender, const std::string& text);

#if PROTOCOL_VERSION < 759
    virtual void Handle(ProtocolCraft::ClientboundChatPacket& msg) override;
#else
    virtual void Handle(ProtocolCraft::ClientboundPlayerChatPacket& msg) override;
    virtual void Handle(ProtocolCraft::ClientboundSystemChatPacket& msg) override;
#endif
    virtual void Handle(ProtocolCraft::ClientboundRespawnPacket& msg) override;

private:
    std::mutex chatMutex;
    std::vector<ChatMsg> chatHistory;
    static const size_t MAX_CHAT_HISTORY = 200;

    mutable std::mutex log_mutex;
    std::deque<std::pair<long long, std::string>> log;
    std::atomic<long long> log_counter;

    void ProcessChatMsg(const std::vector<std::string>& splitted_msg);
};
