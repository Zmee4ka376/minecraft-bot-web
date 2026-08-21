#pragma once

#include <string>
#include <vector>

class WebBotClient;

/// @brief Optional mining module ported from the standalone ChatCommandClient
/// fork (C:/123321). Self-contained on purpose:
///   - all logic lives in MineModule.hpp / MineModule.cpp,
///   - integration with the rest of the bot is limited to
///     * one hook call at the top of WebBotClient::ProcessChatMsg,
///     * one Handle(Respawn) override calling MineModule::NotifyTransfer(),
///     * two source entries in CMakeLists.txt.
/// Remove those to build and test the bot without this module.
namespace MineModule
{
    /// @brief Try to handle a chat command. Commands consumed by the module:
    ///   mine <block> [radius], sortdebris, seallava|zalav [radius],
    ///   place_block <item> x y z, check_perimeter [args], use [chest|shest] <slot>
    /// @return true if the command was handled here (caller should stop processing)
    bool ProcessCommand(WebBotClient& client, const std::vector<std::string>& splitted_msg);

    /// @brief Notify the module about a server transfer (Respawn packet).
    /// Mining pauses for a few seconds afterwards so the anti-cheat does not
    /// flag instant digging right after a join.
    void NotifyTransfer();

    /// @brief True while the "keep sealing lava around me" mode is active.
    bool IsSealLavaActive();
}
