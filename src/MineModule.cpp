#define _USE_MATH_DEFINES
#include <atomic>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <limits>

#include "botcraft/Game/World/World.hpp"
#include "botcraft/Game/World/Blockstate.hpp"
#include "botcraft/Game/AssetsManager.hpp"
#include "botcraft/Game/Entities/EntityManager.hpp"
#include "botcraft/Game/Entities/LocalPlayer.hpp"
#include "botcraft/Game/Inventory/InventoryManager.hpp"
#include "botcraft/Game/Inventory/Window.hpp"
#include "botcraft/Network/NetworkManager.hpp"

#include "botcraft/AI/BehaviourTree.hpp"
#include "botcraft/AI/Tasks/AllTasks.hpp"
#include "botcraft/Utilities/ItemUtilities.hpp"

#include "WebBotClient.hpp"
#include "MineModule.hpp"

using namespace Botcraft;
using namespace ProtocolCraft;

namespace
{
    // --- Module-local state (kept here so WebBotClient stays untouched) ---

    std::atomic<bool> seal_lava_active{ false };

    std::atomic<std::chrono::steady_clock::time_point> last_transfer_time{
        std::chrono::steady_clock::time_point{} };

    bool IsSealLavaActiveLocal()
    {
        return seal_lava_active.load();
    }

    bool InTransferGraceLocal()
    {
        const auto t = last_transfer_time.load();
        if (t == std::chrono::steady_clock::time_point{})
        {
            return false;
        }
        return (std::chrono::steady_clock::now() - t) < std::chrono::seconds(8);
    }
namespace
{
    std::string GetMineBlacklistKey(const std::string& target_name)
    {
        return "Mine.blacklist." + target_name;
    }

    /// @brief Search the loaded chunks within radius of the player for
    /// the closest block matching target_name, skipping blacklisted
    /// positions. Calls Yield() regularly so "stop" can interrupt the scan.
    /// If all_candidates is not null, every non-blacklisted match is
    /// appended there so the result can be cached and reused without
    /// rescanning the world for every single block.
    bool FindNearestBlock(WebBotClient& c, const std::string& target_name,
        const int radius, const std::vector<Position>& blacklist, Position& result,
        std::vector<Position>* all_candidates = nullptr,
        const int y_min = std::numeric_limits<int>::min(),
        const int y_max = std::numeric_limits<int>::max())
    {
        std::shared_ptr<LocalPlayer> local_player = c.GetEntityManager()->GetLocalPlayer();
        if (!local_player)
        {
            return false;
        }

        const Vector3<double> player_pos = local_player->GetPosition();
        const Position center(
            static_cast<int>(std::floor(player_pos.x)),
            static_cast<int>(std::floor(player_pos.y)),
            static_cast<int>(std::floor(player_pos.z))
        );

        const std::shared_ptr<World> world = c.GetWorld();

        bool found = false;
        int candidates = 0;
        long long best_score = std::numeric_limits<long long>::max();
        std::chrono::steady_clock::time_point next_yield = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

        Position current_position;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            current_position.y = center.y + dy;
            for (int dx = -radius; dx <= radius; ++dx)
            {
                current_position.x = center.x + dx;
                for (int dz = -radius; dz <= radius; ++dz)
                {
                    current_position.z = center.z + dz;

                    if (dx * dx + dy * dy + dz * dz > radius * radius)
                    {
                        continue;
                    }

                    const Blockstate* block = world->GetBlock(current_position);

                    if (block == nullptr || block->GetName() != target_name)
                    {
                        continue;
                    }

                    if (current_position.y < y_min || current_position.y > y_max)
                    {
                        continue;
                    }

                    if (std::find(blacklist.begin(), blacklist.end(), current_position) != blacklist.end())
                    {
                        continue;
                    }

                    candidates++;
                    if (all_candidates != nullptr)
                    {
                        all_candidates->push_back(current_position);
                    }
                    // Simply pick the closest block: even 1 block closer
                    // wins, no matter if it needs tunneling or not
                    const long long dist = dx * dx + dy * dy + dz * dz;
                    if (dist < best_score)
                    {
                        best_score = dist;
                        result = current_position;
                        found = true;
                    }
                }
            }

            // Keep the behaviour interruptible during long scans
            if (std::chrono::steady_clock::now() > next_yield)
            {
                c.Yield();
                next_yield = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
        }

        if (found)
        {
            LOG_INFO("[SCAN] " << target_name << ": " << candidates << " candidate(s) in r=" << radius
                << " around " << center << ", best " << result << " (" << std::sqrt(static_cast<double>(best_score)) << " blocks away)");
        }
        else
        {
            LOG_INFO("[SCAN] " << target_name << ": nothing in r=" << radius << " around " << center);
        }

        return found;
    }

    /// Survival block interaction range
    constexpr double DIG_RANGE = 4.5;

    /// @brief Check the target block is visible from the eyes, i.e. no
    /// solid block in between. Used to avoid "x-ray" digging through walls.
    bool HasLineOfSight(World& world, const std::shared_ptr<LocalPlayer>& player, const Position& target)
    {
        const Vector3<double> eyes = player->GetPosition() + Vector3<double>(0.0, player->GetEyeHeight(), 0.0);
        const Vector3<double> center = Vector3<double>(target.x + 0.5, target.y + 0.5, target.z + 0.5);
        const Vector3<double> direction = center - eyes;
        const double distance = std::sqrt(direction.SqrDist(Vector3<double>()));
        Position hit_pos, hit_normal;
        const Blockstate* hit = world.Raycast(eyes, direction, static_cast<float>(distance), hit_pos, hit_normal);
        return hit == nullptr || hit_pos == target;
    }

    /// @brief Check if the target block is within survival digging range
    bool IsInDigRange(World& world, const std::shared_ptr<LocalPlayer>& player, const Position& target)
    {
        const Blockstate* block = world.GetBlock(target);
        if (block == nullptr || block->IsAir())
        {
            return false;
        }
        const Vector3<double> eyes = player->GetPosition() + Vector3<double>(0.0, player->GetEyeHeight(), 0.0);
        return eyes.SqrDist(block->GetClosestPoint(target, eyes)) < DIG_RANGE * DIG_RANGE;
    }

    /// @brief Check if digging pos would open into an adjacent fluid (lava/water)
    bool HasFluidNeighbour(World& world, const Position& pos)
    {
        static const std::array<Position, 6> offsets = {
            Position(1, 0, 0), Position(-1, 0, 0), Position(0, 1, 0),
            Position(0, -1, 0), Position(0, 0, 1), Position(0, 0, -1)
        };
        for (const Position& offset : offsets)
        {
            const Blockstate* neighbour = world.GetBlock(pos + offset);
            if (neighbour != nullptr && neighbour->IsFluid())
            {
                return true;
            }
        }
        return false;
    }

    /// @brief Like HasFluidNeighbour, but covering the full 3x3x3 cube.
    /// Fluids don't spread diagonally in one step, but lava placed
    /// diagonally (especially diagonally above) flows into a freshly
    /// dug cell in two steps (down, then cardinal) — which is exactly
    /// how the bot gets burned standing in a just-opened pocket.
    bool HasFluidIn3x3(World& world, const Position& pos)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    const Blockstate* neighbour = world.GetBlock(pos + Position(dx, dy, dz));
                    if (neighbour != nullptr && neighbour->IsFluid())
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /// @brief Check if standing at pos would be safe: no fluid at pos/head
    /// and solid ground (or at most a small survivable drop) below
    bool IsSafeToStand(World& world, const Position& pos)
    {
        const Blockstate* feet = world.GetBlock(pos);
        const Blockstate* head = world.GetBlock(pos + Position(0, 1, 0));
        if ((feet != nullptr && feet->IsFluid()) || (head != nullptr && head->IsFluid()))
        {
            return false;
        }
        Position below = pos + Position(0, -1, 0);
        for (int depth = 0; depth < 4; ++depth)
        {
            const Blockstate* block = world.GetBlock(below);
            if (block == nullptr)
            {
                return false; // not loaded, don't risk it
            }
            if (block->IsFluid())
            {
                return false; // would fall into lava/water
            }
            if (block->IsSolid())
            {
                return true;
            }
            below.y -= 1;
        }
        return false; // fall would be deeper than 3 blocks
    }

    enum class TunnelStatus
    {
        Arrived, // in range and visible, ready to dig the target
        Moved,   // dug one more tunnel step and advanced
        Blocked  // hazard or unbreakable block in the way, give up this target
    };

    // Same order as the offsets used in PlaceBlockImpl
    const std::array<Position, 6> face_offsets = {
        Position(0, 1, 0), Position(0, -1, 0), Position(0, 0, 1),
        Position(0, 0, -1), Position(1, 0, 0), Position(-1, 0, 0)
    };

    /// Cheap blocks used to seal lava, in order of preference
    const std::array<const char*, 8> FILLER_BLOCKS = {
        "minecraft:netherrack", "minecraft:cobblestone", "minecraft:blackstone",
        "minecraft:basalt", "minecraft:cobbled_deepslate", "minecraft:stone",
        "minecraft:dirt", "minecraft:andesite"
    };

    /// @brief Try to fill the given lava cells with cheap blocks from the
    /// inventory, placing into the lava cell by clicking the face of a
    /// solid neighbour. Returns the number of cells sealed. Cells that
    /// can't be sealed are remembered to avoid repeated multi-second
    /// timeouts.
    int SealLavaCells(WebBotClient& c, const std::vector<Position>& lava_cells, const Vector3<double>& ref)
    {
        if (lava_cells.empty())
        {
            return 0;
        }
        std::shared_ptr<World> world = c.GetWorld();
        if (!world)
        {
            return 0;
        }

        // Find something cheap to place
        std::string filler;
        {
            std::shared_ptr<InventoryManager> inventory_manager = c.GetInventoryManager();
            if (inventory_manager && inventory_manager->GetPlayerInventory())
            {
                std::vector<std::string> available;
                const auto slots = inventory_manager->GetPlayerInventory()->GetSlots();
                for (const auto& [idx, slot] : slots)
                {
                    if (!slot.IsEmptySlot())
                    {
                        const auto& items = AssetsManager::getInstance().Items();
                        const auto it = items.find(slot.GetItemId());
                        if (it != items.end())
                        {
                            available.push_back(it->second->GetName());
                        }
                    }
                }
                for (const char* candidate : FILLER_BLOCKS)
                {
                    if (std::find(available.begin(), available.end(), std::string(candidate)) != available.end())
                    {
                        filler = candidate;
                        break;
                    }
                }
            }
        }

        if (filler.empty())
        {
            // Log only once per session: the seal loop calls this constantly
            if (!c.GetBlackboard().Get<bool>("Mine.no_filler_warned", false))
            {
                c.GetBlackboard().Set<bool>("Mine.no_filler_warned", true);
                LOG_INFO("[LAVA] can't seal lava at " << lava_cells.front() << ": no filler blocks in inventory");
                c.LogMessage("[LAVA] нет блоков для закладки! Дай незеррак/булыжник/чернокамень");
            }
            return 0;
        }

        int sealed = 0;
        for (const Position& lava : lava_cells)
        {
            if (sealed >= 12)
            {
                break;
            }
            // Only cells within interaction range (~4.5 blocks) can be reached
            const Vector3<double> center(lava.x + 0.5, lava.y + 0.5, lava.z + 0.5);
            if (ref.SqrDist(center) > 20.0)
            {
                continue;
            }

            // Place into the lava cell, clicking the face of a solid neighbour.
            // wait_confirmation=false: don't stall up to 3s per block — the seal
            // loop re-scans every iteration and retries anything that remains.
            for (int face_idx = 0; face_idx < 6; ++face_idx)
            {
                const Blockstate* neighbour = world->GetBlock(lava + face_offsets[face_idx]);
                if (neighbour == nullptr || !neighbour->IsSolid())
                {
                    continue;
                }
                if (PlaceBlock(c, filler, lava, static_cast<PlayerDiggingFace>(face_idx), false, false, false) == Status::Success)
                {
                    sealed++;
                    LOG_INFO("[LAVA] sealed lava at " << lava << " with " << filler);
                    break;
                }
            }
        }

        return sealed;
    }

    void EquipBestPickaxe(WebBotClient& c);

    /// @brief Directly swim out of lava using raw movement inputs. The
    /// pathfinder refuses to route through lava (it treats lava as
    /// hazardous), so once the bot is standing IN lava it cannot pathfind
    /// out — it has to move by hand. Looks toward the nearest non-lava cell
    /// and swims (jump = swim up in lava, forward = move toward it) until
    /// the feet and head are clear, or a short timeout expires.
    void FleeLavaDirect(WebBotClient& c)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return;
        }

        auto is_lava = [&world](const Position& pos) -> bool
        {
            const Blockstate* b = world->GetBlock(pos);
            return b != nullptr && b->GetName() == "minecraft:lava";
        };

        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const Vector3<double>& p = player->GetPosition();
            const Position feet(
                static_cast<int>(std::floor(p.x)),
                static_cast<int>(std::floor(p.y)),
                static_cast<int>(std::floor(p.z))
            );

            // Clear of lava at both feet and head?
            if (!is_lava(feet) && !is_lava(feet + Position(0, 1, 0)))
            {
                return;
            }

            // Pick the nearest cell that is not solid and not a fluid,
            // strongly preferring to go up (toward the surface).
            Position best = feet + Position(0, 1, 0);
            double best_score = std::numeric_limits<double>::max();
            for (int dy = -1; dy <= 3; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    for (int dz = -2; dz <= 2; ++dz)
                    {
                        if (dx == 0 && dz == 0 && dy == 0)
                        {
                            continue;
                        }
                        const Position pos = feet + Position(dx, dy, dz);
                        const Blockstate* b = world->GetBlock(pos);
                        if (b == nullptr || b->IsSolid() || b->IsFluid())
                        {
                            continue;
                        }
                        const Blockstate* above = world->GetBlock(pos + Position(0, 1, 0));
                        if (above != nullptr && (above->IsSolid() || above->IsFluid()))
                        {
                            continue;
                        }
                        const double score = static_cast<double>(dx * dx + dy * dy + dz * dz) - (dy > 0 ? 3.0 * dy : 0.0);
                        if (score < best_score)
                        {
                            best_score = score;
                            best = pos;
                        }
                    }
                }
            }

            // Swim toward it for a few physics ticks
            player->LookAt(Vector3<double>(best.x + 0.5, best.y + 0.5, best.z + 0.5), true);
            for (int i = 0; i < 10; ++i)
            {
                player->SetInputsJump(true);
                player->SetInputsForward(1.0f);
                c.Yield();
            }
            player->SetInputsJump(false);
            player->SetInputsForward(0.0f);
        }
    }

    /// @brief EMERGENCY-ONLY lava handling. The bot never seals lava
    /// proactively — it routes around it instead. Sealing happens only
    /// when lava already touches the bot's own cells (e.g. a block was
    /// broken and lava flowed in), and in that case the bot flees FIRST
    /// and patches SECOND. Survival has priority over any loot.
    void SealNearbyLava(WebBotClient& c)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return;
        }

        const Vector3<double>& p = player->GetPosition();
        const Position feet(
            static_cast<int>(std::floor(p.x)),
            static_cast<int>(std::floor(p.y)),
            static_cast<int>(std::floor(p.z))
        );

        // Only lava in the immediate 3x3x3 around the feet is an
        // emergency. Anything further away must be routed around, never
        // touched.
        std::vector<Position> lava_cells;
        Position pos;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    pos = feet + Position(dx, dy, dz);
                    const Blockstate* block = world->GetBlock(pos);
                    if (block != nullptr && block->GetName() == "minecraft:lava")
                    {
                        lava_cells.push_back(pos);
                    }
                }
            }
        }

        if (lava_cells.empty())
        {
            return;
        }

        // Closest lava first, it's the one flowing toward us
        std::sort(lava_cells.begin(), lava_cells.end(),
            [&p](const Position& a, const Position& b)
            {
                return p.SqrDist(Vector3<double>(a.x + 0.5, a.y + 0.5, a.z + 0.5))
                    < p.SqrDist(Vector3<double>(b.x + 0.5, b.y + 0.5, b.z + 0.5));
            });

        LOG_INFO("[LAVA] EMERGENCY: " << lava_cells.size() << " lava cell(s) touching the bot at " << feet
            << ", closest " << lava_cells.front());

        // Lava already in our own feet/head cell: swim out first — sealing
        // while standing IN lava costs health. After we are clear we seal
        // the lava below so it stops flowing toward us.
        const bool in_own_cells = std::find(lava_cells.begin(), lava_cells.end(), feet) != lava_cells.end()
            || std::find(lava_cells.begin(), lava_cells.end(), feet + Position(0, 1, 0)) != lava_cells.end();
        if (in_own_cells)
        {
            LOG_INFO("[LAVA] lava IN the bot's cells at " << feet << ", swimming out then sealing");
            c.LogMessage("[MINE] лава в клетке! Выплываю и закрою!");
            // Pathfinding refuses to move through lava (hazardous), so use
            // raw inputs to swim out instead of GoTo (which just stands there)
            FleeLavaDirect(c);
            // Never keep a filler block in hand while fleeing
            EquipBestPickaxe(c);
        }

        // Seal the lava cells with blocks from the inventory so the flow
        // stops, then IMMEDIATELY put the pickaxe back in the hand —
        // standing around with a filler block equipped near lava is deadly
        SealLavaCells(c, lava_cells, player->GetPosition());
        EquipBestPickaxe(c);

        // Re-read the position (it changed if we just swam out) before the
        // final "still touching" check below
        const Vector3<double> p_now = player->GetPosition();
        const Position feet_now(
            static_cast<int>(std::floor(p_now.x)),
            static_cast<int>(std::floor(p_now.y)),
            static_cast<int>(std::floor(p_now.z))
        );

        // If lava is still touching after sealing, back off
        bool still_touching = false;
        for (const Position& lava : lava_cells)
        {
            if (world->GetBlock(lava) != nullptr && world->GetBlock(lava)->GetName() == "minecraft:lava"
                && std::abs(lava.x - feet_now.x) <= 1 && std::abs(lava.z - feet_now.z) <= 1
                && lava.y >= feet_now.y - 1 && lava.y <= feet_now.y + 1)
            {
                still_touching = true;
                break;
            }
        }
        if (still_touching)
        {
            // Walk away from the closest lava cell
            const Position& closest = lava_cells.front();
            const int away_x = closest.x >= feet_now.x ? -1 : 1;
            const int away_z = closest.z >= feet_now.z ? -1 : 1;
            const Position flee(feet_now.x + away_x * 8, feet_now.y, feet_now.z + away_z * 8);
            LOG_INFO("[LAVA] lava still touching after sealing, backing off from " << closest << " toward " << flee << ", bot at " << feet_now);
            c.LogMessage("[MINE] лава рядом, отступаю");
            GoTo(c, flee, 2);
            EquipBestPickaxe(c);
        }
    }

    /// @brief Seal lava directly around the given cell (e.g. behind a
    /// block the tunnel wants to dig through), so the dig can proceed
    /// instead of the whole target being blocked. Covers the 3x3x3
    /// neighbourhood: diagonal sources can flow in two steps too.
    void SealLavaAround(WebBotClient& c, const Position& around)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return;
        }
        std::vector<Position> lava_cells;
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    if (dx == 0 && dy == 0 && dz == 0)
                    {
                        continue;
                    }
                    const Blockstate* block = world->GetBlock(around + Position(dx, dy, dz));
                    if (block != nullptr && block->GetName() == "minecraft:lava")
                    {
                        lava_cells.push_back(around + Position(dx, dy, dz));
                    }
                }
            }
        }
        if (!lava_cells.empty())
        {
            LOG_INFO("[LAVA] sealing " << lava_cells.size() << " lava cell(s) around " << around << " to unblock the tunnel");
            SealLavaCells(c, lava_cells, player->GetPosition());
        }
    }

    /// @brief True if the bot has an unobstructed view of the given lava
    /// cell, i.e. no solid wall between the eyes and the cell. Lava itself
    /// has no collider, so the ray is stopped just short of the cell: any
    /// hit means there is a wall in the way.
    /// @brief Standalone "seal lava" routine, exposed as a command. Scans a
    /// cube of the given radius around the bot's feet for lava and fills
    /// every cell it can reach with a cheap block from the inventory.
    /// Returns the number of cells sealed. Silent when there is nothing to
    /// seal, so the toggle loop doesn't spam the log.
    int SealLavaAroundBot(WebBotClient& c, const int radius)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return 0;
        }

        const Vector3<double>& p = player->GetPosition();
        const Position feet(
            static_cast<int>(std::floor(p.x)),
            static_cast<int>(std::floor(p.y)),
            static_cast<int>(std::floor(p.z))
        );

        std::vector<Position> lava_cells;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dz = -radius; dz <= radius; ++dz)
                {
                    const Position pos = feet + Position(dx, dy, dz);
                    const Blockstate* b = world->GetBlock(pos);
                    if (b != nullptr && b->GetName() == "minecraft:lava")
                    {
                        lava_cells.push_back(pos);
                    }
                }
            }
        }

        if (lava_cells.empty())
        {
            return 0;
        }

        // Nearest first, so the lava closest to the bot is sealed first
        std::sort(lava_cells.begin(), lava_cells.end(),
            [&p](const Position& a, const Position& b)
            {
                return p.SqrDist(Vector3<double>(a.x + 0.5, a.y + 0.5, a.z + 0.5))
                    < p.SqrDist(Vector3<double>(b.x + 0.5, b.y + 0.5, b.z + 0.5));
            });

        const int sealed = SealLavaCells(c, lava_cells, p);
        EquipBestPickaxe(c);

        if (sealed > 0)
        {
            LOG_INFO("[LAVA] sealed " << sealed << " lava cell(s) near " << feet);
            c.LogMessage("[LAVA] закрыто " + std::to_string(sealed));
        }
        return sealed;
    }

    /// @brief Fraction of remaining durability of the held item
    /// (1.0 = intact). Returns 1.0 if it's not a durability tool.
    double HeldToolDurabilityFraction(WebBotClient& c)
    {
        std::shared_ptr<InventoryManager> inv = c.GetInventoryManager();
        if (!inv)
        {
            return 1.0;
        }
        const ProtocolCraft::Slot hand = inv->GetHotbarSelected();
        if (hand.IsEmptySlot())
        {
            return 1.0;
        }
        const auto& items = AssetsManager::getInstance().Items();
        const auto it = items.find(hand.GetItemId());
        if (it == items.end() || it->second->GetMaxDurability() <= 0)
        {
            return 1.0;
        }
        const int damage = Utilities::GetDamageCount(hand);
        return 1.0 - static_cast<double>(damage) / it->second->GetMaxDurability();
    }

    Status MineNextBlock(WebBotClient& c, const std::string& target_name, const int radius);

    void EquipBestPickaxe(WebBotClient& c);

    TunnelStatus TunnelStep(WebBotClient& c, const Position& target);

    /// @brief Dig with a retry limit: if the same position fails to break
    /// 3 times in a row (e.g. a server anticheat silently refusing the
    /// finish-digging packet), stop hammering it for the session
    Status DigGuarded(WebBotClient& c, const Position& pos)
    {
        const std::string fails_key = "Mine.digfails";
        std::vector<std::pair<Position, int>> fails = c.GetBlackboard().Get<std::vector<std::pair<Position, int>>>(fails_key, {});

        int count = 0;
        for (const auto& [p, n] : fails)
        {
            if (p == pos)
            {
                count = n;
                break;
            }
        }
        if (count >= 3)
        {
            return Status::Failure; // already given up on this one
        }

        // Lava sealing / eating can leave a filler block or food in the
        // hand: without this the server computes hand-speed dig times
        // (seconds per block instead of a fraction of a second)
        EquipBestPickaxe(c);

        // Full traceability: what we break, where, and how long it took
        std::string block_name = "?";
        {
            const Blockstate* b = c.GetWorld()->GetBlock(pos);
            if (b != nullptr)
            {
                block_name = b->GetName();
            }
        }
        const auto dig_start = std::chrono::steady_clock::now();
        LOG_INFO("[DIG] breaking " << block_name << " at " << pos);

        // The real dig task (NOT a recursive call to itself!)
        const Status result = Dig(c, pos, true, PlayerDiggingFace::Up, false);

        if (result == Status::Success)
        {
            LOG_INFO("[DIG] broke " << block_name << " at " << pos << " ("
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - dig_start).count() << "ms)");
        }

        if (result == Status::Failure)
        {
            count++;
            LOG_INFO("[DIG] attempt " << count << "/3 failed to break " << pos);
            bool updated = false;
            for (auto& [p, n] : fails)
            {
                if (p == pos)
                {
                    n = count;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                fails.push_back({ pos, count });
            }
            if (count >= 3)
            {
                LOG_INFO("[MINE] block at " << pos << " refuses to break (" << count << " attempts), skipping it");
                c.LogMessage("[MINE] блок не ломается (античит?), пропускаю");
            }
            c.GetBlackboard().Set<std::vector<std::pair<Position, int>>>(fails_key, fails);
        }
        else if (count > 0)
        {
            // It finally broke, forget the failures
            std::vector<std::pair<Position, int>> cleaned;
            for (const auto& [p, n] : fails)
            {
                if (!(p == pos))
                {
                    cleaned.push_back({ p, n });
                }
            }
            c.GetBlackboard().Set<std::vector<std::pair<Position, int>>>(fails_key, cleaned);
        }
        return result;
    }


    /// Pickaxes from best to worst
    const std::array<const char*, 6> PICKAXE_RANKS = {
        "minecraft:netherite_pickaxe", "minecraft:diamond_pickaxe",
        "minecraft:golden_pickaxe", "minecraft:iron_pickaxe",
        "minecraft:stone_pickaxe", "minecraft:wooden_pickaxe"
    };

    /// @brief Make sure the best available pickaxe is in the hotbar and
    /// selected, so we never dig with the hand or a random filler block
    void EquipBestPickaxe(WebBotClient& c)
    {
        std::shared_ptr<InventoryManager> inv = c.GetInventoryManager();
        std::shared_ptr<Window> win = inv ? inv->GetPlayerInventory() : nullptr;
        if (!win)
        {
            return;
        }

        std::string hand_name;
        {
            const ProtocolCraft::Slot hand = inv->GetHotbarSelected();
            if (!hand.IsEmptySlot())
            {
                const auto& items = AssetsManager::getInstance().Items();
                const auto it = items.find(hand.GetItemId());
                if (it != items.end())
                {
                    hand_name = it->second->GetName();
                }
            }
        }

        auto slot_name_at = [&win](const short s) -> std::string {
            const ProtocolCraft::Slot slot = win->GetSlot(s);
            if (slot.IsEmptySlot())
            {
                return "";
            }
            const auto& items = AssetsManager::getInstance().Items();
            const auto it = items.find(slot.GetItemId());
            return it == items.end() ? "" : it->second->GetName();
        };

        for (const char* pick_name : PICKAXE_RANKS)
        {
            for (short s = 9; s <= 44; ++s)
            {
                if (slot_name_at(s) != pick_name)
                {
                    continue;
                }
                // This is the best pickaxe we have
                if (hand_name == pick_name)
                {
                    return; // already holding it
                }
                if (s >= 36)
                {
                    c.SelectHotbarSlot(s - 36);
                    return;
                }
                // Move it from the main inventory into the hotbar
                short dest = -1;
                for (short h = 36; h <= 44; ++h)
                {
                    if (win->GetSlot(h).IsEmptySlot())
                    {
                        dest = h;
                        break;
                    }
                }
                if (dest == -1)
                {
                    dest = 36; // no empty hotbar slot, swap with the first one
                }
                c.SwapSlots(Window::PLAYER_INVENTORY_INDEX, s, dest);
                c.SelectHotbarSlot(dest - 36);
                return;
            }
        }

        // No pickaxe anywhere and not holding one: warn once per session
        if (hand_name.find("pickaxe") == std::string::npos)
        {
            if (!c.GetBlackboard().Get<bool>("Mine.no_pickaxe_warned", false))
            {
                c.GetBlackboard().Set<bool>("Mine.no_pickaxe_warned", true);
                LOG_INFO("[MINE] no pickaxe in the inventory, digging will be very slow!");
                c.LogMessage("[MINE] нет кирки в инвентаре — копаю чем есть!");
            }
        }
    }

    /// @brief Check if there is any pickaxe anywhere in the inventory
    bool HasAnyPickaxe(WebBotClient& c)
    {
        std::shared_ptr<InventoryManager> inv = c.GetInventoryManager();
        std::shared_ptr<Window> win = inv ? inv->GetPlayerInventory() : nullptr;
        if (!win)
        {
            return false;
        }
        auto slot_name_at = [&win](const short s) -> std::string {
            const ProtocolCraft::Slot slot = win->GetSlot(s);
            if (slot.IsEmptySlot())
            {
                return "";
            }
            const auto& items = AssetsManager::getInstance().Items();
            const auto it = items.find(slot.GetItemId());
            return it == items.end() ? "" : it->second->GetName();
        };
        for (short s = 9; s <= 44; ++s)
        {
            const std::string name = slot_name_at(s);
            if (name.find("pickaxe") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void CollectNearbyDrops(WebBotClient& c, const Position& around);

    /// @brief Wait until a pickaxe appears in the inventory, at most
    /// timeout_seconds. Returns true if one was received in time.
    /// A pickaxe lost to a desynced inventory click often lies on the
    /// ground right next to the bot: periodically vacuum nearby drops.
    bool WaitForPickaxe(WebBotClient& c, const int timeout_seconds)
    {
        auto last_report = std::chrono::steady_clock::now();
        auto last_vacuum = std::chrono::steady_clock::now();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        while (!HasAnyPickaxe(c))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            if (std::chrono::steady_clock::now() - last_report > std::chrono::seconds(30))
            {
                last_report = std::chrono::steady_clock::now();
                LOG_INFO("[REPAIR] still waiting for a pickaxe...");
            }
            if (std::chrono::steady_clock::now() - last_vacuum > std::chrono::seconds(10))
            {
                last_vacuum = std::chrono::steady_clock::now();
                // While standing here we're exposed: keep any nearby lava
                // sealed so the wait itself can't kill the bot
                SealNearbyLava(c);
                std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
                if (player)
                {
                    const Vector3<double>& p = player->GetPosition();
                    CollectNearbyDrops(c, Position(
                        static_cast<int>(std::floor(p.x)),
                        static_cast<int>(std::floor(p.y)),
                        static_cast<int>(std::floor(p.z))));
                }
            }
            // Yield so "stop" still works, then idle a bit
            for (int i = 0; i < 20; ++i)
            {
                c.Yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    }

    /// @brief If the held pickaxe durability is low, mine nether quartz
    /// for XP until mending repairs the tool above 95%, so it never breaks.
    /// If there is no pickaxe at all (lost/broken after a death), wait a
    /// few minutes for one instead of wrongly assuming "100% durability".
    /// Returns false when mining must stop: nobody gave a pickaxe in
    /// time, or the last one is about to break and can't be repaired.
    bool MaintainPickaxe(WebBotClient& c, const int radius)
    {
        if (!HasAnyPickaxe(c))
        {
            LOG_INFO("[REPAIR] no pickaxe in inventory, pausing until one is given");
            c.LogMessage("[MINE] нет кирки! Дай новую — я жду и продолжу");
            if (!WaitForPickaxe(c, 180))
            {
                LOG_INFO("[REPAIR] no pickaxe given within 3 minutes, stopping the mining session");
                c.LogMessage("[MINE] кирки так и не дали, останавливаюсь");
                return false;
            }
            LOG_INFO("[REPAIR] pickaxe received, resuming");
            c.LogMessage("[MINE] кирка получена, продолжаю!");
            EquipBestPickaxe(c);
            return true;
        }

        if (HeldToolDurabilityFraction(c) >= 0.20)
        {
            return true;
        }

        LOG_INFO("[MINE] pickaxe below 20% durability, mining quartz until repaired (mending)");
        c.LogMessage("[MINE] кирка изношена, копаю кварц для ремонта");
        int safety = 0;
        int no_progress = 0;
        while (HeldToolDurabilityFraction(c) < 0.95 && safety++ < 200)
        {
            if (!HasAnyPickaxe(c))
            {
                // The pickaxe was lost (death?) during the repair, wait for a new one
                if (!WaitForPickaxe(c, 180))
                {
                    LOG_INFO("[REPAIR] no pickaxe given within 3 minutes, stopping the mining session");
                    c.LogMessage("[MINE] кирки так и не дали, останавливаюсь");
                    return false;
                }
                EquipBestPickaxe(c);
                no_progress = 0;
                continue;
            }
            const double before = HeldToolDurabilityFraction(c);
            if (MineNextBlock(c, "minecraft:nether_quartz_ore", radius) == Status::Failure)
            {
                LOG_INFO("[REPAIR] no quartz found nearby, resuming without full repair");
                break;
            }
            // Mending must actually recover durability while mining quartz.
            // If it doesn't for several blocks in a row, the "mined" results
            // were fake (unreachable targets reported as success) or the
            // pickaxe has no mending at all — stop wasting time.
            if (HeldToolDurabilityFraction(c) > before)
            {
                no_progress = 0;
            }
            else if (++no_progress >= 6)
            {
                LOG_INFO("[REPAIR] durability is not recovering (" << no_progress << " quartz in a row), resuming");
                break;
            }
            LOG_INFO("[REPAIR] pickaxe durability now " << static_cast<int>(HeldToolDurabilityFraction(c) * 100) << "%");
        }

        if (HeldToolDurabilityFraction(c) < 0.05)
        {
            // Digging on would destroy the pickaxe for good
            LOG_INFO("[REPAIR] pickaxe under 5% and not repairable, stopping the mining session");
            c.LogMessage("[MINE] кирка на исходе и ремонт не идёт — останавливаюсь, дай кирку с Mending!");
            return false;
        }
        LOG_INFO("[MINE] pickaxe durability " << static_cast<int>(HeldToolDurabilityFraction(c) * 100) << "%, back to work");
        return true;
    }

    /// @brief Move all ancient_debris stacks into inventory row 2 (slots
    /// 18-26): everything merged into the first slot, then exactly one
    /// debris placed into each remaining cell of the row
    void SortDebrisIntoRow(WebBotClient& c)
    {
        std::shared_ptr<InventoryManager> inv = c.GetInventoryManager();
        if (!inv || !inv->GetPlayerInventory())
        {
            return;
        }
        constexpr short row_start = 18;
        constexpr short row_end = 26;
        const std::string debris = "minecraft:ancient_debris";

        auto slot_name = [](const ProtocolCraft::Slot& s) -> std::string {
            if (s.IsEmptySlot())
            {
                return "";
            }
            const auto& items = AssetsManager::getInstance().Items();
            const auto it = items.find(s.GetItemId());
            return it == items.end() ? "" : it->second->GetName();
        };

        // --- Phase 1: gather everything into the row, merge into the first slot ---
        for (int pass = 0; pass < 60; ++pass)
        {
            std::array<ProtocolCraft::Slot, 46> slots{};
            for (short i = 9; i <= 44; ++i)
            {
                slots[i] = inv->GetPlayerInventory()->GetSlot(i);
            }

            // 1) Something else sits in the row: move it to an empty slot outside
            for (short t = row_start; t <= row_end; ++t)
            {
                const std::string name = slot_name(slots[t]);
                if (name.empty() || name == debris)
                {
                    continue;
                }
                for (short e = 9; e <= 44; ++e)
                {
                    if ((e >= row_start && e <= row_end) || !slots[e].IsEmptySlot())
                    {
                        continue;
                    }
                    c.SwapSlots(Window::PLAYER_INVENTORY_INDEX, t, e);
                    goto next_pass;
                }
            }

            // 2) Debris outside the row: move it into an empty row slot
            for (short src = 9; src <= 44; ++src)
            {
                if ((src >= row_start && src <= row_end) || slot_name(slots[src]) != debris)
                {
                    continue;
                }
                for (short t = row_start; t <= row_end; ++t)
                {
                    if (!slots[t].IsEmptySlot())
                    {
                        continue;
                    }
                    c.SwapSlots(Window::PLAYER_INVENTORY_INDEX, src, t);
                    goto next_pass;
                }
                // Row is full of debris, go merge instead
                break;
            }

            // 3) Merge partial stacks into the first slot of the row
            // (sum <= 64 so no remainder is left in the hand)
            for (short a = row_start + 1; a <= row_end; ++a)
            {
                if (slot_name(slots[a]) != debris || slots[a].GetItemCount() >= 64)
                {
                    continue;
                }
                if (slot_name(slots[row_start]) != debris
                    || slots[row_start].GetItemCount() + slots[a].GetItemCount() > 64)
                {
                    continue;
                }
                // Pick up a, place onto the first row slot
                if (c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, a, 0, 0)
                    && c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, row_start, 0, 0))
                {
                    LOG_INFO("[SORT] merged debris slot " << a << " into " << row_start);
                }
                goto next_pass;
            }
            break; // gathering done

        next_pass:;
        }

        // --- Phase 2: spread exactly one debris into every remaining row cell ---
        for (int pass = 0; pass < 20; ++pass)
        {
            std::array<ProtocolCraft::Slot, 46> slots{};
            for (short i = 9; i <= 44; ++i)
            {
                slots[i] = inv->GetPlayerInventory()->GetSlot(i);
            }

            for (short t = row_start + 1; t <= row_end; ++t)
            {
                if (!slots[t].IsEmptySlot())
                {
                    continue;
                }
                short source = -1;
                int source_count = 0;
                for (short a = row_start; a <= row_end; ++a)
                {
                    if (a != t && slot_name(slots[a]) == debris && slots[a].GetItemCount() > source_count)
                    {
                        source = a;
                        source_count = slots[a].GetItemCount();
                    }
                }
                if (source == -1)
                {
                    return; // nothing to spread anymore
                }
                if (source_count == 1)
                {
                    c.SwapSlots(Window::PLAYER_INVENTORY_INDEX, source, t);
                }
                else if (source_count == 2)
                {
                    // Right-click pickup takes 1 of 2, place 1, hand ends up empty
                    c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, source, 0, 1);
                    c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, t, 0, 1);
                }
                else
                {
                    // Take half, place exactly one (right click), return the rest
                    c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, source, 0, 1);
                    c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, t, 0, 1);
                    c.ClickSlot(Window::PLAYER_INVENTORY_INDEX, source, 0, 0);
                }
                goto next_spread;
            }
            break; // no empty cells left

        next_spread:;
        }
        c.LogMessage("[SORT] ancient_debris разложен по 2-й строчке инвентаря");
    }

    /// @brief Walk toward a single drop, digging a 1x2 tunnel if needed,
    /// until the bot is within the server pickup radius (~1 block).
    /// @param tunnel_budget Shared limit of tunnel steps across all drops:
    /// chasing a stray item is a nice-to-have, the mining loop must not get
    /// stuck digging a long tunnel or bouncing between two items.
    /// @return true if the bot ended up close enough to pick the drop up.
    bool ReachDrop(WebBotClient& c, const Position& item, int& tunnel_budget)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!player || !world)
        {
            return false;
        }

        // Cheap path first: existing passages, no digging
        if (GoTo(c, item, 1) == Status::Success)
        {
            return true;
        }

        // Plain pathfinding failed: dig a minimal walkable path, one 1x2
        // step at a time, stopping as soon as the drop is within pickup
        // range. No look-ahead here — the multi-block look-ahead used for
        // mining keeps digging past a nearby drop and makes the bot bounce
        // back and forth around it.
        for (int attempt = 0; attempt < 6 && tunnel_budget > 0; ++attempt)
        {
            const Vector3<double>& p = player->GetPosition();
            const Position current(
                static_cast<int>(std::floor(p.x)),
                static_cast<int>(std::floor(p.y + 0.25)),
                static_cast<int>(std::floor(p.z))
            );
            const int manhattan = std::abs(item.x - current.x) + std::abs(item.y - current.y) + std::abs(item.z - current.z);
            if (manhattan <= 1)
            {
                GoTo(c, item, 1);
                return true;
            }

            SealNearbyLava(c);
            EquipBestPickaxe(c);

            // One cardinal step toward the item. If we are exactly
            // above/below it, it is too deep to be worth a shaft — give up.
            const int dx = item.x - current.x;
            const int dz = item.z - current.z;
            Position dest = current;
            if (dx == 0 && dz == 0)
            {
                return false;
            }
            else if (std::abs(dx) >= std::abs(dz))
            {
                dest.x += (dx > 0) - (dx < 0);
            }
            else
            {
                dest.z += (dz > 0) - (dz < 0);
            }

            // Make the destination standable: clear feet + head cells.
            const std::array<Position, 2> cells = { dest, dest + Position(0, 1, 0) };
            for (const Position& cell : cells)
            {
                const Blockstate* b = world->GetBlock(cell);
                if (b == nullptr || b->IsAir())
                {
                    continue;
                }
                if (b->IsFluid() || b->GetHardness() < 0.0f || HasFluidIn3x3(*world, cell))
                {
                    return false;
                }
                if (DigGuarded(c, cell) == Status::Failure)
                {
                    return false;
                }
            }

            tunnel_budget--;
            if (GoTo(c, dest, 0) == Status::Failure)
            {
                return false;
            }
        }
        return false;
    }

    /// @brief Walk onto nearby dropped item entities to pick them up
    /// (drops from the block we just mined at around)
    void CollectNearbyDrops(WebBotClient& c, const Position& around)
    {
        std::shared_ptr<EntityManager> entity_manager = c.GetEntityManager();
        if (!entity_manager)
        {
            return;
        }
        std::shared_ptr<LocalPlayer> player = entity_manager->GetLocalPlayer();
        if (!player)
        {
            return;
        }
        const Vector3<double> player_pos = player->GetPosition();

        std::vector<Position> item_positions;
        {
            auto entities = entity_manager->GetEntities();
            for (const auto& [id, entity] : *entities)
            {
                if (entity == nullptr || entity->GetType() != EntityType::ItemEntity)
                {
                    continue;
                }
                const Vector3<double>& p = entity->GetPosition();
                // Close to the mined block AND close enough to the bot so
                // we don't chase drops into unreachable holes
                if (std::abs(p.x - around.x) <= 3.0 && std::abs(p.y - around.y) <= 3.0 && std::abs(p.z - around.z) <= 3.0
                    && player_pos.SqrDist(p) < 64.0)
                {
                    const Position block_pos(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));
                    if (std::find(item_positions.begin(), item_positions.end(), block_pos) == item_positions.end())
                    {
                        item_positions.push_back(block_pos);
                    }
                }
            }
        }

        // Closest drop first: the one we just produced sits right at the
        // mined block, stray items are farther away and not worth a detour.
        std::sort(item_positions.begin(), item_positions.end(),
            [&around](const Position& a, const Position& b)
            {
                const int da = std::abs(a.x - around.x) + std::abs(a.y - around.y) + std::abs(a.z - around.z);
                const int db = std::abs(b.x - around.x) + std::abs(b.y - around.y) + std::abs(b.z - around.z);
                return da < db;
            });

        if (!item_positions.empty())
        {
            LOG_INFO("[PICKUP] collecting " << item_positions.size() << " drop(s) near " << around);
        }

        // Shared budget: at most a few tunnel steps for ALL drops combined,
        // so the bot never digs a long tunnel or bounces between two items.
        int tunnel_budget = 5;
        for (const Position& item : item_positions)
        {
            if (tunnel_budget <= 0)
            {
                break;
            }

            // Still there?
            bool still = false;
            for (const auto& [id, entity] : *entity_manager->GetEntities())
            {
                if (entity == nullptr || entity->GetType() != EntityType::ItemEntity)
                {
                    continue;
                }
                const Vector3<double>& p = entity->GetPosition();
                if (std::abs(p.x - item.x) <= 1.0 && std::abs(p.y - item.y) <= 1.0 && std::abs(p.z - item.z) <= 1.0)
                {
                    still = true;
                    break;
                }
            }
            if (!still)
            {
                continue; // already picked up
            }
            if (c.GetWorld() != nullptr && HasFluidIn3x3(*c.GetWorld(), item))
            {
                LOG_INFO("[PICKUP] skipping drop at " << item << " (lava around it)");
                continue;
            }
            ReachDrop(c, item, tunnel_budget);
        }
    }

    /// Common food items, in order of preference
    const std::array<const char*, 18> FOOD_NAMES = {
        "minecraft:golden_apple", "minecraft:cooked_beef", "minecraft:cooked_porkchop",
        "minecraft:bread", "minecraft:cooked_mutton", "minecraft:cooked_chicken",
        "minecraft:cooked_salmon", "minecraft:cooked_cod", "minecraft:baked_potato",
        "minecraft:golden_carrot", "minecraft:carrot", "minecraft:apple",
        "minecraft:beetroot", "minecraft:melon_slice", "minecraft:pumpkin_pie",
        "minecraft:sweet_berries", "minecraft:glow_berries", "minecraft:dried_kelp"
    };

    /// @brief If the food level dropped below max, eat something from
    /// the inventory (called between mining two blocks)
    void EatIfHungry(WebBotClient& c)
    {
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!player || player->GetFood() >= 20)
        {
            return;
        }

        std::string food_name;
        std::shared_ptr<InventoryManager> inventory_manager = c.GetInventoryManager();
        if (inventory_manager && inventory_manager->GetPlayerInventory())
        {
            std::vector<std::string> available;
            const auto slots = inventory_manager->GetPlayerInventory()->GetSlots();
            for (const auto& [idx, slot] : slots)
            {
                if (slot.IsEmptySlot())
                {
                    continue;
                }
                const auto& items = AssetsManager::getInstance().Items();
                const auto it = items.find(slot.GetItemId());
                if (it != items.end())
                {
                    available.push_back(it->second->GetName());
                }
            }

            for (const char* candidate : FOOD_NAMES)
            {
                if (std::find(available.begin(), available.end(), std::string(candidate)) != available.end())
                {
                    food_name = candidate;
                    break;
                }
            }
        }

        if (food_name.empty())
        {
            LOG_INFO("[MINE] hungry (food " << player->GetFood() << "/20) but no food found in inventory");
            return;
        }

        LOG_INFO("[MINE] hungry (food " << player->GetFood() << "/20), eating " << food_name);
        if (Eat(c, food_name) == Status::Failure)
        {
            LOG_INFO("[MINE] eating " << food_name << " failed");
        }
    }

    /// @brief Baritone-style tunneling: dig one 1x2 tunnel step toward the
    /// target (with 1/1 staircases when the height differs) and walk into it.
    /// The target itself is only dug when visible (no digging through walls).
    TunnelStatus TunnelStep(WebBotClient& c, const Position& target)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!player)
        {
            return TunnelStatus::Blocked;
        }

        if (IsInDigRange(*world, player, target) && HasLineOfSight(*world, player, target))
        {
            return TunnelStatus::Arrived;
        }

        // In dig range but the line of sight is blocked by another block:
        // remove that obstruction directly instead of stepping around it —
        // stepping oscillates back and forth next to the target
        if (IsInDigRange(*world, player, target))
        {
            const Vector3<double> eyes = player->GetPosition() + Vector3<double>(0.0, player->GetEyeHeight(), 0.0);
            const Vector3<double> center(target.x + 0.5, target.y + 0.5, target.z + 0.5);
            const Vector3<double> direction = center - eyes;
            const double distance = std::sqrt(direction.SqrDist(Vector3<double>()));
            Position hit_pos, hit_normal;
            const Blockstate* hit = world->Raycast(eyes, direction, static_cast<float>(distance), hit_pos, hit_normal);
            if (hit != nullptr && hit_pos != target && !hit->IsFluid() && hit->GetHardness() >= 0.0f)
            {
                if (!HasFluidIn3x3(*world, hit_pos) && DigGuarded(c, hit_pos) == Status::Success)
                {
                    return TunnelStatus::Moved; // the target may be visible now
                }
            }
        }

        const Vector3<double> position = player->GetPosition();
        // Same cell convention as GoToImpl (floor(y + 0.25)): on partial
        // blocks like soul sand (top at y+0.875) the two formulas disagree
        // by a whole cell, and a staircase dug for "down 1" is then a
        // "down 2" for the pathfinder — no path, instant give-up
        const Position current(
            static_cast<int>(std::floor(position.x)),
            static_cast<int>(std::floor(position.y + 0.25)),
            static_cast<int>(std::floor(position.z))
        );

        const int dx = target.x - current.x;
        const int dy = target.y - current.y;
        const int dz = target.z - current.z;

        // Horizontal step toward the target, along the axis with the most
        // ground left to cover
        Position step(0, 0, 0);
        if (std::abs(dx) >= std::abs(dz))
        {
            step.x = (dx > 0) - (dx < 0);
        }
        else
        {
            step.z = (dz > 0) - (dz < 0);
        }

        // Directly above/below the target: carve a staircase instead of a
        // vertical shaft
        if (step.x == 0 && step.z == 0)
        {
            step.x = 1;
        }

        // Staircase up/down when the target is not roughly at the same height.
        // The pathfinder requires:
        // - up: a solid block at (front, y) to jump on, headroom above our
        //   own head, and the two cells above it dug,
        // - down: three cells dug ((front, y+1), (front, y), (front, y-1))
        //   with a solid floor at (front, y-2).
        int step_y = 0;
        if (dy >= 2)
        {
            step_y = 1;
        }
        else if (dy <= -2)
        {
            step_y = -1;
        }
        // No solid block to jump on for the up staircase: walk level instead
        if (step_y == 1)
        {
            const Blockstate* stair_block = world->GetBlock(current + step);
            if (stair_block == nullptr || stair_block->IsAir())
            {
                step_y = 0;
            }
        }
        // No solid floor to stand on for the down staircase: walk level instead
        if (step_y == -1)
        {
            const Blockstate* floor_block = world->GetBlock(current + step + Position(0, -2, 0));
            if (floor_block == nullptr || floor_block->IsAir())
            {
                step_y = 0;
            }
        }
        // Directly under the target with nothing to climb on: a level walk
        // can never shrink dy, and stepping sideways just to step back on
        // the next iteration makes the bot pace back and forth under the
        // target. Report the target unreachable from here instead.
        if (step_y == 0 && dy >= 2 && dx == 0 && dz == 0)
        {
            LOG_INFO("[TUNNEL] target " << target << " is straight above with no block to climb on, bot at " << current);
            return TunnelStatus::Blocked;
        }

        Position destination = current + step;
        std::vector<Position> cells;
        if (step_y == 1)
        {
            // Jump on the solid (front, y) block, land with feet at (front, y+1)
            destination = current + Position(step.x, 1, step.z);
            cells = {
                current + Position(0, 2, 0),        // headroom to jump
                destination,                        // (front, y+1)
                destination + Position(0, 1, 0)     // (front, y+2)
            };
        }
        else if (step_y == -1)
        {
            // Drop into (front, y-1), floor at (front, y-2) stays solid
            destination = current + Position(step.x, -1, step.z);
            cells = {
                current + Position(step.x, 1, step.z), // (front, y+1)
                current + step,                        // (front, y)
                destination                            // (front, y-1)
            };
        }
        else
        {
            // Level 1x2 tunnel, floor at (front, y-1) stays solid
            cells = { destination, destination + Position(0, 1, 0) };
        }

        for (const Position& cell : cells)
        {
            const Blockstate* block = world->GetBlock(cell);
            if (block == nullptr || block->IsAir())
            {
                continue;
            }
            // Bedrock / unbreakable, or the cell itself is a fluid
            if (block->IsFluid() || block->GetHardness() < 0.0f)
            {
                LOG_INFO("[TUNNEL] blocked at " << cell << " ("
                    << (block->IsFluid() ? "fluid" : "unbreakable")
                    << "), bot at " << current << ", target " << target);
                return TunnelStatus::Blocked;
            }
            // Any lava around the cell means we don't touch it at all:
            // route around, never try to seal proactively
            if (HasFluidIn3x3(*world, cell))
            {
                LOG_INFO("[TUNNEL] blocked at " << cell << " (lava around), bot at " << current << ", target " << target);
                return TunnelStatus::Blocked;
            }
            // Adjacent cell, no pathfinding needed
            if (DigGuarded(c, cell) == Status::Failure)
            {
                LOG_INFO("[TUNNEL] dig failed at " << cell << ", bot at " << current << ", target " << target);
                return TunnelStatus::Blocked;
            }
        }

        if (!IsSafeToStand(*world, destination))
        {
            LOG_INFO("[TUNNEL] unsafe to step into " << destination << " (fall or fluid below), bot at " << current << ", target " << target);
            return TunnelStatus::Blocked;
        }

        // Look-ahead: on a level tunnel, break the next few cells along
        // the step direction while they are still within dig range, and
        // walk through them in one go — one GoTo per several blocks
        // instead of a pathfinding walk for every single cell
        Position walk_end = destination;
        if (step_y == 0)
        {
            Position far = destination;
            for (int ahead = 0; ahead < 3; ++ahead)
            {
                const Position next = far + Position(step.x, 0, step.z);
                // Never dig/walk past the target's column: overshooting
                // flips the horizontal direction on the next step and the
                // bot paces back and forth across the target
                if ((step.x != 0 && (next.x - target.x) * step.x > 0)
                    || (step.z != 0 && (next.z - target.z) * step.z > 0))
                {
                    break;
                }
                bool ok = true;
                for (const Position& cell : { next, next + Position(0, 1, 0) })
                {
                    const Blockstate* b = world->GetBlock(cell);
                    if (b == nullptr || b->IsAir())
                    {
                        continue;
                    }
                    if (b->IsFluid() || b->GetHardness() < 0.0f || HasFluidIn3x3(*world, cell))
                    {
                        ok = false;
                        break;
                    }
                    if (!IsInDigRange(*world, player, cell) || !HasLineOfSight(*world, player, cell))
                    {
                        ok = false;
                        break;
                    }
                    if (DigGuarded(c, cell) == Status::Failure)
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok || !IsSafeToStand(*world, next))
                {
                    break;
                }
                far = next;
            }
            walk_end = far;
        }

        if (GoTo(c, walk_end, 0) == Status::Failure && walk_end != destination)
        {
            // Long batched walk didn't work out, try the plain single step
            walk_end = destination;
        }
        if (GoTo(c, walk_end, 0) == Status::Failure)
        {
            LOG_INFO("[TUNNEL] can't walk into " << walk_end << ", bot at " << current << ", target " << target);
            return TunnelStatus::Blocked;
        }

        return TunnelStatus::Moved;
    }

    /// @brief Single mining step: find the closest remaining block of the
    /// given type, walk as close as possible using existing passages, then
    /// dig a Baritone-style tunnel toward it and mine it. Returns Failure
    /// when there is nothing left to mine, which ends the mining loop.
    /// Blocks that can't be reached safely are blacklisted and skipped.
    /// @brief Dig a straight level tunnel n blocks in the direction the
    /// bot is facing, to load new chunks before rescanning. Turns 90°
    /// when blocked. Returns the number of steps dug.
    int DigForward(WebBotClient& c, const int n)
    {
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!player)
        {
            return 0;
        }
        const float yaw = player->GetYaw() * static_cast<float>(M_PI) / 180.0f;
        const float fx = -std::sin(yaw);
        const float fz = std::cos(yaw);
        int step_x = std::abs(fx) >= std::abs(fz) ? (fx > 0.0f ? 1 : -1) : 0;
        int step_z = step_x == 0 ? (fz > 0.0f ? 1 : -1) : 0;

        auto make_virtual_target = [&player]() -> Position {
            const Vector3<double> p = player->GetPosition();
            return Position(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y + 0.25)), static_cast<int>(std::floor(p.z)));
        };

        Position here = make_virtual_target();
        Position virtual_target = here + Position(step_x * 64, 0, step_z * 64);
        LOG_INFO("[DIGFWD] digging " << n << " blocks toward (" << step_x << ",0," << step_z << ") from " << here);

        int dug = 0;
        int rotations = 0;
        for (int i = 0; i < n && rotations <= 3; ++i)
        {
            SealNearbyLava(c);
            EquipBestPickaxe(c);

            const TunnelStatus s = TunnelStep(c, virtual_target);
            if (s == TunnelStatus::Arrived || s == TunnelStatus::Moved)
            {
                dug++;
                continue;
            }
            // Blocked: rotate 90° and keep going in the new direction
            rotations++;
            const int new_x = -step_z;
            const int new_z = step_x;
            step_x = new_x;
            step_z = new_z;
            here = make_virtual_target();
            virtual_target = here + Position(step_x * 64, 0, step_z * 64);
        }
        LOG_INFO("[DIGFWD] done, dug " << dug << "/" << n << " blocks, bot now at " << make_virtual_target());
        return dug;
    }

    /// @brief After breaking a block, greedily break all connected blocks
    /// of the same type (vein/cluster mining, diagonals included), without
    /// going back to the full world scan between blocks
    void MineVein(WebBotClient& c, const std::string& target_name, const Position& start)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return;
        }

        std::vector<Position> to_check;
        std::vector<Position> visited = { start };
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    if (dx != 0 || dy != 0 || dz != 0)
                    {
                        to_check.push_back(start + Position(dx, dy, dz));
                    }
                }
            }
        }

        int broken = 0;
        Position last_broken = start;
        LOG_INFO("[VEIN] found vein of " << target_name << " at " << start << ", mining the connected blocks");
        while (!to_check.empty() && broken < 32 && visited.size() < 128)
        {
            const Position pos = to_check.back();
            to_check.pop_back();
            if (std::find(visited.begin(), visited.end(), pos) != visited.end())
            {
                continue;
            }
            visited.push_back(pos);

            const Blockstate* block = world->GetBlock(pos);
            if (block == nullptr || block->GetName() != target_name)
            {
                continue;
            }

            // Never chase vein blocks that sit next to lava: walking to
            // them is exactly how the bot gets burned. Survival first,
            // the block stays in the ground.
            if (HasFluidIn3x3(*world, pos))
            {
                LOG_INFO("[VEIN] skipping " << target_name << " at " << pos << " (lava around it)");
                continue;
            }

            // Only break blocks we can reach right now. Walking to a vein
            // block between every single break is exactly the "think 1-2s"
            // delay — leave out-of-range blocks to the main loop, which
            // walks/tunnels to them properly.
            if (!IsInDigRange(*world, player, pos) || !HasLineOfSight(*world, player, pos))
            {
                continue;
            }

            SealNearbyLava(c);
            EquipBestPickaxe(c);

            if (DigGuarded(c, pos) == Status::Success)
            {
                broken++;
                last_broken = pos;
                LOG_INFO("[VEIN] broke block " << broken << " of the " << target_name << " vein at " << pos
                    << ", " << to_check.size() << " neighbour(s) still to check");
                for (int dx = -1; dx <= 1; ++dx)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dz = -1; dz <= 1; ++dz)
                        {
                            if (dx != 0 || dy != 0 || dz != 0)
                            {
                                to_check.push_back(pos + Position(dx, dy, dz));
                            }
                        }
                    }
                }
            }
        }

        if (broken > 0)
        {
            LOG_INFO("[MINE] vein mined " << broken << " extra " << target_name << " blocks near " << start);
            c.LogMessage("[MINE] жила: +" + std::to_string(broken) + " " + target_name);
            // Quartz is mined only for the mending XP (absorbed on the
            // spot); its items are worthless, so don't walk around picking
            // them up — that is the "thinking" pause between blocks.
            if (target_name != "minecraft:nether_quartz_ore")
            {
                CollectNearbyDrops(c, last_broken);
            }
        }
    }

    /// @brief Check the blocks right around the bot for the target type
    /// (e.g. passing them in a tunnel) and dig any within reach.
    /// Returns true if at least one was dug.
    bool TryDigAdjacent(WebBotClient& c, const std::string& target_name)
    {
        std::shared_ptr<World> world = c.GetWorld();
        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        if (!world || !player)
        {
            return false;
        }
        const Vector3<double>& p = player->GetPosition();
        const Position feet(
            static_cast<int>(std::floor(p.x)),
            static_cast<int>(std::floor(p.y)),
            static_cast<int>(std::floor(p.z))
        );

        for (int dy = -1; dy <= 2; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    if (dx == 0 && dy == 0 && dz == 0)
                    {
                        continue;
                    }
                    const Position pos = feet + Position(dx, dy, dz);
                    const Blockstate* block = world->GetBlock(pos);
                    if (block == nullptr || block->GetName() != target_name)
                    {
                        continue;
                    }
                    if (!IsInDigRange(*world, player, pos) || !HasLineOfSight(*world, player, pos))
                    {
                        continue;
                    }
                    // Never break an ore with lava above/below/beside it —
                    // the hole lets the lava pour onto us
                    if (HasFluidIn3x3(*world, pos))
                    {
                        LOG_INFO("[MINE] opportunistic skip: " << target_name << " at " << pos << " has lava around it");
                        continue;
                    }
                    SealNearbyLava(c);
                    EquipBestPickaxe(c);
                    if (DigGuarded(c, pos) == Status::Success)
                    {
                        LOG_INFO("[MINE] opportunistic " << target_name << " at " << pos);
                        MineVein(c, target_name, pos);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    Status MineNextBlock(WebBotClient& c, const std::string& target_name, const int radius)
    {
        // Grace period after a server transfer: stand still for a few seconds
        // so the anti-cheat doesn't interpret immediate mining as a cheat.
        if (InTransferGraceLocal())
        {
            c.Yield();
            return Status::Success; // retry on next tick
        }

        // Recursion guard: MineNextBlock <-> MaintainPickaxe can nest
        // (debris -> repair quartz -> ...). Beyond a small depth something
        // went wrong — log it and bail out instead of overflowing the stack.
        static thread_local int mine_depth = 0;
        if (mine_depth >= 3)
        {
            LOG_WARNING("[MINE] recursion guard hit (depth " << mine_depth << ") while mining " << target_name);
            return Status::Failure;
        }
        struct DepthGuard
        {
            int& counter;
            ~DepthGuard() { counter--; }
        } depth_guard{ ++mine_depth };

        // Tool maintenance: don't mine the repair ore recursively,
        // everything else checks the pickaxe first
        if (target_name != "minecraft:nether_quartz_ore")
        {
            EquipBestPickaxe(c);
            if (!MaintainPickaxe(c, radius))
            {
                return Status::Failure;
            }
        }

        // After a death/respawn the old blacklists make no sense anymore
        if (c.GetBlackboard().Get<bool>("Mine.needs_reset", false))
        {
            c.GetBlackboard().Set<bool>("Mine.needs_reset", false);
            c.GetBlackboard().Set<std::vector<Position>>(GetMineBlacklistKey(target_name), {});
            c.GetBlackboard().Set<std::vector<Position>>(GetMineBlacklistKey(target_name) + ".temp", {});
            c.GetBlackboard().Set<std::vector<Position>>(GetMineBlacklistKey(target_name) + ".walkfailed", {});
            c.GetBlackboard().Set<std::vector<Position>>(GetMineBlacklistKey(target_name) + ".cache", {});
            c.GetBlackboard().Set<std::vector<Position>>("Mine.lava_seal_failed", {});
            c.GetBlackboard().Set<std::vector<std::pair<Position, int>>>("Mine.digfails", {});
            LOG_INFO("[MINE] respawn detected, blacklists cleared");
        }

        const std::string blacklist_key = GetMineBlacklistKey(target_name);
        const std::string temp_key = blacklist_key + ".temp";
        const std::string cache_key = blacklist_key + ".cache";
        const std::string cache_center_key = cache_key + ".center";
        std::vector<Position> blacklist = c.GetBlackboard().Get<std::vector<Position>>(blacklist_key, {});
        std::vector<Position> temp_blacklist = c.GetBlackboard().Get<std::vector<Position>>(temp_key, {});

        // Ancient debris only generates at y 8..22. Blocks higher up
        // (exposed in big caverns) are reachable only through long
        // staircases — stay in the productive band unless explicitly
        // asked to mine something else
        const bool is_debris = target_name == "minecraft:ancient_debris";
        const int band_y_min = is_debris ? 5 : std::numeric_limits<int>::min();
        const int band_y_max = is_debris ? 25 : std::numeric_limits<int>::max();

        const std::shared_ptr<World> world = c.GetWorld();
        auto read_bot_pos = [&c]() -> Position
        {
            std::shared_ptr<LocalPlayer> p = c.GetEntityManager()->GetLocalPlayer();
            if (!p)
            {
                return Position(0, 0, 0);
            }
            const Vector3<double>& pos = p->GetPosition();
            return Position(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), static_cast<int>(std::floor(pos.z)));
        };

        // Candidates cached from a previous scan: picking the nearest from
        // this list is far cheaper than rescanning the whole sphere after
        // every mined block. Entries that were mined/blacklisted in the
        // meantime are pruned on the fly.
        std::vector<Position> cache = c.GetBlackboard().Get<std::vector<Position>>(cache_key, {});
        const Position cache_center = c.GetBlackboard().Get<Position>(cache_center_key, Position(0, std::numeric_limits<int>::min(), 0));
        auto pick_from_cache = [&](Position& out, const char* context) -> bool
        {
            if (world == nullptr || cache.empty())
            {
                return false;
            }
            const Position bot = read_bot_pos();
            // Cheap dig-cost estimate: how many solid blocks sit on the
            // straight line to the candidate. A nearby ore behind thick
            // rock is worse than a slightly farther one reachable through
            // an existing tunnel, so pick by (dig cost, distance)
            auto dig_cost = [&](const Position& p) -> int
            {
                const int steps = std::max({ std::abs(p.x - bot.x), std::abs(p.y - bot.y), std::abs(p.z - bot.z) });
                if (steps == 0)
                {
                    return 0;
                }
                int solids = 0;
                for (int s = 1; s <= steps; ++s)
                {
                    const Position cell(
                        static_cast<int>(std::floor(bot.x + (p.x - bot.x) * static_cast<double>(s) / steps + 0.5)),
                        static_cast<int>(std::floor(bot.y + (p.y - bot.y) * static_cast<double>(s) / steps + 0.5)),
                        static_cast<int>(std::floor(bot.z + (p.z - bot.z) * static_cast<double>(s) / steps + 0.5))
                    );
                    const Blockstate* b = world->GetBlock(cell);
                    if (b != nullptr && !b->IsAir() && !b->IsFluid())
                    {
                        solids++;
                    }
                }
                return solids;
            };

            // Collect the selectable candidates with their distances
            std::vector<std::pair<long long, Position>> selectable;
            std::vector<Position> kept;
            kept.reserve(cache.size());
            for (const Position& p : cache)
            {
                const bool perm_blacklisted = std::find(blacklist.begin(), blacklist.end(), p) != blacklist.end();
                const bool temp_blacklisted = std::find(temp_blacklist.begin(), temp_blacklist.end(), p) != temp_blacklist.end();
                const Blockstate* b = world->GetBlock(p);
                if (b == nullptr || b->IsAir() || b->GetName() != target_name)
                {
                    continue; // mined meanwhile, forget it
                }
                if (!perm_blacklisted)
                {
                    kept.push_back(p);
                }
                if (perm_blacklisted || temp_blacklisted)
                {
                    continue; // kept for later (temp), but not selectable now
                }
                if (p.y < band_y_min || p.y > band_y_max)
                {
                    continue; // out of the productive band
                }
                const long long dx = p.x - bot.x, dy = p.y - bot.y, dz = p.z - bot.z;
                selectable.push_back({ dx * dx + dy * dy + dz * dz, p });
            }
            cache.swap(kept);
            c.GetBlackboard().Set<std::vector<Position>>(cache_key, cache);
            if (selectable.empty())
            {
                return false;
            }
            std::sort(selectable.begin(), selectable.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            // Among the nearest candidates, prefer the one needing the
            // least digging (the tunnel is 1x2, so each solid step costs 2 digs)
            long long best_score = std::numeric_limits<long long>::max();
            bool best_found = false;
            for (size_t i = 0; i < selectable.size() && i < 8; ++i)
            {
                const Position& p = selectable[i].second;
                // Distance weight is on par with digging cost: a straight
                // line through a huge cave can look "free" while the real
                // route is a long staircase
                const long long score = 2LL * dig_cost(p) + static_cast<long long>(std::sqrt(static_cast<double>(selectable[i].first)));
                if (score < best_score)
                {
                    best_score = score;
                    out = p;
                    best_found = true;
                }
            }
            if (best_found)
            {
                LOG_INFO("[TARGET] " << context << " " << target_name << " at " << out << " (score " << best_score
                    << ", " << cache.size() << " candidates left in cache)");
            }
            return best_found;
        };

        // The cache describes the world around the point it was scanned at:
        // once we've wandered away from there, rescan instead
        auto cache_is_fresh = [&]() -> bool
        {
            if (cache.empty() || cache_center.y == std::numeric_limits<int>::min())
            {
                return false;
            }
            const Position bot = read_bot_pos();
            const long long dx = cache_center.x - bot.x, dy = cache_center.y - bot.y, dz = cache_center.z - bot.z;
            return dx * dx + dy * dy + dz * dz <= 32 * 32;
        };

        // Find a target, growing the search radius, then digging forward
        // into unloaded territory if there is nothing around
        const int max_steps = std::max(128, radius * 3);
        Position target;
        bool found = false;
        int search_radius = radius;
        if (cache_is_fresh() && pick_from_cache(target, "picked (cached)"))
        {
            found = true; // cheap path: reuse the cached candidate list
        }
        for (int attempt = 0; attempt < 30 && !found; ++attempt)
        {
            std::vector<Position> combined = blacklist;
            combined.insert(combined.end(), temp_blacklist.begin(), temp_blacklist.end());
            std::vector<Position> all_candidates;
            if (FindNearestBlock(c, target_name, search_radius, combined, target, &all_candidates, band_y_min, band_y_max))
            {
                found = true;
                // Remember every candidate so the next blocks don't rescan
                c.GetBlackboard().Set<std::vector<Position>>(cache_key, all_candidates);
                c.GetBlackboard().Set<Position>(cache_center_key, read_bot_pos());
                break;
            }
            if (search_radius < 160)
            {
                search_radius = std::min(160, search_radius * 3 / 2);
                LOG_INFO("[MINE] nothing found, growing search radius to " << search_radius);
                continue;
            }
            // Nothing in the loaded chunks, dig forward to load new ones.
            // The new chunks are not in the cache anymore: drop it.
            LOG_INFO("[MINE] no " << target_name << " within " << search_radius << " blocks, digging 20 blocks forward");
            c.GetBlackboard().Set<std::vector<Position>>(cache_key, {});
            cache.clear();
            if (DigForward(c, 20) == 0)
            {
                LOG_INFO("[MINE] can't dig forward either, stopping the search");
                break;
            }
            search_radius = radius;
        }

        if (!found)
        {
            // Give temporarily postponed targets another chance before
            // ending the session
            if (!temp_blacklist.empty())
            {
                temp_blacklist.clear();
                c.GetBlackboard().Set<std::vector<Position>>(temp_key, temp_blacklist);
                if (pick_from_cache(target, "retrying postponed")
                    || FindNearestBlock(c, target_name, search_radius, blacklist, target, nullptr, band_y_min, band_y_max))
                {
                    found = true;
                }
            }
            if (!found)
            {
                return Status::Failure;
            }
        }

        // First get as close as possible walking through existing passages,
        // only tunnel for the remaining distance. Don't retry walking to
        // targets where walking already proved useless — also treat nearby
        // positions as known-failed: a wall that blocked one walk attempt
        // blocks its whole neighbourhood, and every pointless pathfinding
        // run costs seconds.
        const std::string walk_failed_key = blacklist_key + ".walkfailed";
        std::vector<Position> walk_failed = c.GetBlackboard().Get<std::vector<Position>>(walk_failed_key, {});
        auto walk_known_failed = [&](const Position& p) -> bool
        {
            for (const Position& w : walk_failed)
            {
                const long long dx = w.x - p.x, dy = w.y - p.y, dz = w.z - p.z;
                if (dx * dx + dy * dy + dz * dz <= 16) // within 4 blocks
                {
                    return true;
                }
            }
            return false;
        };
        // Cheap pre-check: a target fully embedded in solid rock has no
        // air pocket anywhere around it, so walking there can never
        // succeed. Don't burn seconds of pathfinding on it — tunnel
        // directly.
        auto has_air_nearby = [&world](const Position& p) -> bool
        {
            for (int dx = -5; dx <= 5; ++dx)
            {
                for (int dy = -4; dy <= 4; ++dy)
                {
                    for (int dz = -5; dz <= 5; ++dz)
                    {
                        if (dx * dx + dy * dy + dz * dz > 25)
                        {
                            continue;
                        }
                        const Blockstate* b = world->GetBlock(p + Position(dx, dy, dz));
                        if (b == nullptr || b->IsAir() || b->IsFluid())
                        {
                            return true;
                        }
                    }
                }
            }
            return false;
        };
        if (!walk_known_failed(target))
        {
            if (!has_air_nearby(target))
            {
                LOG_INFO("[WALKSKIP] " << target << " is fully embedded in rock, tunneling directly (no walk attempt)");
            }
            else
            {
                LOG_INFO("[WALK] trying to walk close to " << target << " before tunneling");
                if (GoTo(c, target, 4) == Status::Failure)
                {
                    walk_failed.push_back(target);
                    c.GetBlackboard().Set<std::vector<Position>>(walk_failed_key, walk_failed);
                    LOG_INFO("[WALK] walking to " << target << " failed, will tunnel there");
                }
            }
        }
        else
        {
            LOG_INFO("[WALKSKIP] not trying to walk to " << target << " (a nearby walk already failed), tunneling directly");
        }

        // While walking we may have passed closer blocks: re-select the
        // nearest target from the cached candidates (no world rescan)
        {
            Position better;
            if (pick_from_cache(better, "re-selected closer"))
            {
                target = better;
            }
        }

        std::shared_ptr<LocalPlayer> player = c.GetEntityManager()->GetLocalPlayer();
        Vector3<double> last_position = player ? player->GetPosition() : Vector3<double>(0.0, 0.0, 0.0);
        std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();
        // Closest Manhattan distance to the target seen so far. Pacing
        // back and forth is movement, so a "haven't moved" check never
        // fires; lack of improvement in this distance is the real signal.
        int best_dist = 1000000;
        std::chrono::steady_clock::time_point next_heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        LOG_INFO("[MINE] going for " << target_name << " at " << target << ", bot at ("
            << static_cast<int>(std::floor(last_position.x)) << "," << static_cast<int>(std::floor(last_position.y)) << "," << static_cast<int>(std::floor(last_position.z)) << ")");

        Status result = Status::Failure;
        bool stuck = false;
        for (int step = 0; step < max_steps; ++step)
        {
            // Safety first: seal any lava that got close to us
            SealNearbyLava(c);

            // Always dig with the pickaxe, not with the filler block
            // that the lava sealing or food eating left in the hand
            EquipBestPickaxe(c);

            const std::shared_ptr<World> world = c.GetWorld();
            const Blockstate* block = world->GetBlock(target);
            // Already mined, possibly as tunnel collateral if the tunnel
            // crossed another block of the same type
            if (block == nullptr || block->IsAir())
            {
                result = Status::Success;
                break;
            }

            // Opportunistic: dig target blocks right next to us while
            // moving around (e.g. passing them in a tunnel)
            if (TryDigAdjacent(c, target_name))
            {
                continue;
            }

            // Heartbeat so long sessions can be traced in the log
            if (std::chrono::steady_clock::now() > next_heartbeat)
            {
                next_heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                if (player)
                {
                    const Vector3<double>& pp = player->GetPosition();
                    LOG_INFO("[MINE] working: bot at (" << static_cast<int>(std::floor(pp.x)) << ","
                        << static_cast<int>(std::floor(pp.y)) << "," << static_cast<int>(std::floor(pp.z))
                        << "), target " << target << ", step " << step << "/" << max_steps);
                }
            }

            const TunnelStatus tunnel = TunnelStep(c, target);
            if (tunnel == TunnelStatus::Arrived)
            {
                // Never dig out an ore with lava above/below/beside it:
                // breaking it opens a hole the lava pours through. Skip
                // it (blacklisted below) and go for the next one.
                if (HasFluidIn3x3(*world, target))
                {
                    LOG_INFO("[MINE] skipping " << target_name << " at " << target << " (lava above/below/beside it)");
                    c.LogMessage("[MINE] обломок у лавы — пропускаю");
                    break; // falls into the "giving up" branch -> blacklisted
                }
                result = DigGuarded(c, target);
                break;
            }
            if (tunnel == TunnelStatus::Blocked)
            {
                break;
            }

            // Stuck watchdog: oscillating between two spots is movement,
            // so instead of "haven't moved" we track the closest we've
            // ever been to the target. No improvement for 8 seconds ->
            // postpone this target and go for another one
            if (player)
            {
                const Vector3<double> pos_now = player->GetPosition();
                const int dist_now = std::abs(target.x - static_cast<int>(std::floor(pos_now.x)))
                    + std::abs(target.y - static_cast<int>(std::floor(pos_now.y + 0.25)))
                    + std::abs(target.z - static_cast<int>(std::floor(pos_now.z)));
                if (dist_now < best_dist)
                {
                    best_dist = dist_now;
                    last_progress = std::chrono::steady_clock::now();
                }
                else if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_progress).count() >= 8)
                {
                    stuck = true;
                    LOG_INFO("[MINE] no progress for 8s on the way to " << target << " (best distance " << best_dist << "), postponing it");
                    c.LogMessage("[MINE] застрял, беру другую цель");
                    break;
                }
            }
        }

        if (result == Status::Success)
        {
            LOG_INFO("[MINE] mined " << target_name << " at " << target);
            c.LogMessage("[MINE] mined " + target_name + " at "
                + std::to_string(target.x) + " " + std::to_string(target.y) + " " + std::to_string(target.z));
            // Break the whole connected vein/cluster while we're here
            MineVein(c, target_name, target);
            // Pick up the drops before moving on. Quartz items are
            // worthless (mined only for mending XP), so skip the pickup
            // walk — it is the pause the bot "thinks" between blocks.
            if (target_name != "minecraft:nether_quartz_ore")
            {
                CollectNearbyDrops(c, target);
            }
            // Mined something: give postponed targets another chance
            if (!temp_blacklist.empty())
            {
                temp_blacklist.clear();
                c.GetBlackboard().Set<std::vector<Position>>(temp_key, temp_blacklist);
            }
        }
        else if (stuck)
        {
            // Temporary blacklist, cleared after the next successful block
            temp_blacklist.push_back(target);
            c.GetBlackboard().Set<std::vector<Position>>(temp_key, temp_blacklist);
        }
        else
        {
            LOG_INFO("[MINE] giving up on " << target_name << " at " << target << " (unreachable or hazardous)");
            c.LogMessage("[MINE] can't reach " + target_name + " at "
                + std::to_string(target.x) + " " + std::to_string(target.y) + " " + std::to_string(target.z) + ", skipping");
            blacklist.push_back(target);
            c.GetBlackboard().Set<std::vector<Position>>(blacklist_key, blacklist);
        }

        // If hungry, eat before going for the next block
        EatIfHungry(c);

        return Status::Success;
    }

    /// @brief Check for any spawnable blocks in a sphere from pos and write
    /// all the positions into a file. Use check_lighting to add a check on
    /// light block value (> 7) (warning: ignore top slabs and upside-down
    /// stairs, you should check for such blocks manually)
    void CheckPerimeter(WebBotClient& client, const Position& pos, const float radius, const bool check_lighting)
    {
        const std::shared_ptr<World> world = client.GetWorld();
        std::vector<Position> found_positions;

        Position current_position;
        for (int y = static_cast<int>(-radius - 1); y < radius + 1; ++y)
        {
            current_position.y = pos.y + y;
            for (int x = static_cast<int>(-radius - 1); x < radius + 1; ++x)
            {
                current_position.x = pos.x + x;
                for (int z = static_cast<int>(-radius - 1); z < radius + 1; ++z)
                {
                    current_position.z = pos.z + z;

                    if (x * x + y * y + z * z > radius * radius)
                    {
                        continue;
                    }

                    const Blockstate* block = world->GetBlock(current_position);

                    if (block == nullptr || !block->IsAir())
                    {
                        continue;
                    }

                    Position adjacent_position = current_position;
                    adjacent_position.y -= 1;

                    const Blockstate* adjacent_block = world->GetBlock(adjacent_position);

                    if (!adjacent_block ||
                        adjacent_block->IsFluid() ||
                        !adjacent_block->IsSolid() ||
                        adjacent_block->IsTransparent() ||
                        adjacent_block->GetName() == "minecraft:bedrock" ||
                        adjacent_block->GetName() == "minecraft:barrier")
                    {
                        continue;
                    }

                    adjacent_position.y += 2;

                    adjacent_block = world->GetBlock(adjacent_position);

                    if (adjacent_block &&
                        (adjacent_block->IsSolid() ||
                        adjacent_block->IsFluid()))
                    {
                        continue;
                    }

                    if (check_lighting && world->GetBlockLight(current_position) > 7)
                    {
                        continue;
                    }

                    found_positions.push_back(current_position);
                }
            }
        }

        std::ofstream output_file("perimeter_check_" + std::to_string(pos.x) + "_" + std::to_string(pos.y) + "_" + std::to_string(pos.z) + "_radius_" + std::to_string(radius) + ".txt", std::ios::out);

        if (output_file.is_open())
        {
            for (size_t i = 0; i < found_positions.size(); ++i)
            {
                output_file << found_positions[i] << "\n";
            }

            output_file.close();
        }
    }

}
}


namespace MineModule
{
    bool ProcessCommand(WebBotClient& client, const std::vector<std::string>& splitted_msg)
    {
        if (splitted_msg.size() < 2)
        {
            return false;
        }

        // NOTE: "goto", "stop", "interact", "dig" and "die" are handled by
        // WebBotClient itself, this module only adds the mining commands.

        if (splitted_msg[1] == "place_block")
        {
            if (splitted_msg.size() < 6)
            {
                client.SendChatMessage("Usage: [BotName] [place_block] [item] [x] [y] [z]");
                return true;
            }
            const std::string& item = splitted_msg[2];
            Position pos;
            try
            {
                pos = Position(std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4]), std::stoi(splitted_msg[5]));
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
            catch (const std::out_of_range&)
            {
                return true;
            }
            LOG_INFO("Asked to place a block at " << pos << " (" << item << ")");

            auto tree = Builder<WebBotClient>("place block")
                .sequence()
                    .succeeder().leaf(PlaceBlock, item, pos, PlayerDiggingFace::Up, true, true, true)
                    .leaf([](WebBotClient& c) { c.SetBehaviourTree(nullptr); return Status::Success; })
                .end();

            client.SetBehaviourTree(tree);
            return true;
        }
        else if (splitted_msg[1] == "mine")
        {
            if (splitted_msg.size() < 3)
            {
                client.SendChatMessage("Usage: [BotName] [mine] [block_name] [radius=64]");
                return true;
            }

            std::string target_name = splitted_msg[2];
            // Add the default namespace if not specified
            if (target_name.find(':') == std::string::npos)
            {
                target_name = "minecraft:" + target_name;
            }

            int radius = 64;
            if (splitted_msg.size() > 3)
            {
                try
                {
                    radius = std::stoi(splitted_msg[3]);
                }
                catch (const std::invalid_argument&)
                {
                    return true;
                }
                catch (const std::out_of_range&)
                {
                    return true;
                }
                if (radius < 1 || radius > 128)
                {
                    client.SendChatMessage("Radius must be in [1, 128]");
                    return true;
                }
            }

            if (AssetsManager::getInstance().GetBlockstate(target_name) == nullptr)
            {
                client.SendChatMessage("Unknown block: " + target_name);
                return true;
            }

            LOG_INFO("Asked to mine " << target_name << " within " << radius << " blocks");

            const std::string blacklist_key = GetMineBlacklistKey(target_name);

            auto tree = Builder<WebBotClient>("mine " + target_name)
                .sequence()
                    // Reset the blacklists and lava seal memory from any previous mining session
                    .leaf([blacklist_key](WebBotClient& c)
                        {
                            c.GetBlackboard().Set<std::vector<Position>>(blacklist_key, {});
                            c.GetBlackboard().Set<std::vector<Position>>(blacklist_key + ".temp", {});
                            c.GetBlackboard().Set<std::vector<Position>>(blacklist_key + ".walkfailed", {});
                            c.GetBlackboard().Set<std::vector<Position>>(blacklist_key + ".cache", {});
                            c.GetBlackboard().Set<std::vector<Position>>("Mine.lava_seal_failed", {});
                            c.GetBlackboard().Set<std::vector<std::pair<Position, int>>>("Mine.digfails", {});
                            return Status::Success;
                        })
                    .leaf(Say, "Mining " + target_name + ", send 'stop' to cancel")
                    // Repeat until MineNextBlock returns Failure (nothing left to mine):
                    // invert the result so the repeater stops on "no more blocks"
                    .repeater(0)
                        .leaf([=](WebBotClient& c) { return MineNextBlock(c, target_name, radius) == Status::Failure ? Status::Success : Status::Failure; })
                    .leaf(Say, "No more " + target_name + " found nearby")
                    // Switch back to empty behaviour
                    .leaf([](WebBotClient& c) { c.SetBehaviourTree(nullptr); return Status::Success; })
                .end();

            client.SetBehaviourTree(tree);
            return true;
        }
        else if (splitted_msg[1] == "sortdebris")
        {
            SortDebrisIntoRow(client);
            return true;
        }
        else if (splitted_msg[1] == "seallava" || splitted_msg[1] == "zalav")
        {
            int radius = 4;
            if (splitted_msg.size() > 2)
            {
                try
                {
                    radius = std::stoi(splitted_msg[2]);
                }
                catch (const std::exception&)
                {
                    radius = 4;
                }
            }
            if (radius < 1 || radius > 8)
            {
                radius = 4;
            }

            seal_lava_active.store(!seal_lava_active.load());
            if (IsSealLavaActiveLocal())
            {
                client.LogMessage("[CMD] auto-seal lava ON (radius " + std::to_string(radius) + ")");
                // Keep sealing lava around the bot until the flag is turned off.
                // The leaf returns Success when the flag is off, which stops the
                // repeater and the sequence below switches back to no behaviour.
                auto tree = Builder<WebBotClient>("seal lava loop")
                    .sequence()
                        .repeater(0)
                            .leaf([radius](WebBotClient& c)
                                {
                                    if (!IsSealLavaActiveLocal())
                                    {
                                        return Status::Success;
                                    }
                                    SealLavaAroundBot(c, radius);
                                    c.Yield();
                                    return Status::Failure;
                                })
                        .leaf([](WebBotClient& c)
                            {
                                c.LogMessage("[LAVA] seal mode OFF");
                                c.SetBehaviourTree(nullptr);
                                return Status::Success;
                            })
                    .end();
                client.SetBehaviourTree(tree);
            }
            else
            {
                client.LogMessage("[CMD] auto-seal lava OFF");
                client.SetBehaviourTree(nullptr);
            }
            return true;
        }
        else if (splitted_msg[1] == "check_perimeter")
        {
            float radius = 128.0f;
            Position pos = Position(
                static_cast<int>(std::floor(client.GetEntityManager()->GetLocalPlayer()->GetPosition().x)),
                static_cast<int>(std::floor(client.GetEntityManager()->GetLocalPlayer()->GetPosition().y)),
                static_cast<int>(std::floor(client.GetEntityManager()->GetLocalPlayer()->GetPosition().z))
            );
            bool check_lighting = true;

            if (splitted_msg.size() == 3)
            {
                radius = std::stof(splitted_msg[2]);
            }
            else if (splitted_msg.size() == 4)
            {
                radius = std::stof(splitted_msg[2]);
                check_lighting = std::stoi(splitted_msg[3]) != 0;
            }
            else if (splitted_msg.size() == 6)
            {
                pos = Position(std::stoi(splitted_msg[2]), std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4]));
                radius = std::stof(splitted_msg[5]);
            }
            else if (splitted_msg.size() == 7)
            {
                pos = Position(std::stoi(splitted_msg[2]), std::stoi(splitted_msg[3]), std::stoi(splitted_msg[4]));
                radius = std::stof(splitted_msg[5]);
                check_lighting = std::stoi(splitted_msg[6]) != 0;
            }
            CheckPerimeter(client, pos, radius, check_lighting);
            return true;
        }
        else if (splitted_msg[1] == "use")
        {
            // [BotName] use <slot>           -> click a slot in the player inventory
            // [BotName] use shest <slot>     -> click a slot in the opened chest
            const bool in_chest = splitted_msg.size() >= 3 && (splitted_msg[2] == "shest" || splitted_msg[2] == "chest");
            const int slot_arg_index = in_chest ? 3 : 2;
            if (splitted_msg.size() < slot_arg_index + 1)
            {
                client.LogMessage("[CMD] Usage: use <slot> | use chest <slot>");
                client.SendChatMessage("Usage: [BotName] use <slot> | [BotName] use shest <slot>");
                return true;
            }

            int slot;
            try
            {
                slot = std::stoi(splitted_msg[slot_arg_index]);
            }
            catch (const std::invalid_argument&)
            {
                client.LogMessage("[CMD] Invalid slot number");
                return true;
            }
            catch (const std::out_of_range&)
            {
                client.LogMessage("[CMD] Slot number out of range");
                return true;
            }

            short window_id;
            if (in_chest)
            {
                const std::shared_ptr<InventoryManager> inventory_manager = client.GetInventoryManager();
                window_id = inventory_manager ? inventory_manager->GetFirstOpenedWindowId() : -1;
                if (window_id == -1)
                {
                    client.LogMessage("[CMD] No chest is open");
                    client.SendChatMessage("No chest is open");
                    return true;
                }
            }
            else
            {
                window_id = Window::PLAYER_INVENTORY_INDEX;
            }

            client.LogMessage("[CMD] Clicking slot " + std::to_string(slot) + " in window " + std::to_string(window_id));

            if (client.ClickSlot(window_id, static_cast<short>(slot), 0, 0))
            {
                client.LogMessage("[CMD] Clicked slot " + std::to_string(slot) + (in_chest ? " in chest" : ""));
                client.SendChatMessage("Clicked slot " + std::to_string(slot) + (in_chest ? " in chest" : ""));
            }
            else
            {
                client.LogMessage("[CMD] Failed to click slot " + std::to_string(slot));
                client.SendChatMessage("Failed to click slot " + std::to_string(slot));
            }
            return true;
        }

        return false;
    }

    void NotifyTransfer()
    {
        last_transfer_time.store(std::chrono::steady_clock::now());
    }

    bool IsSealLavaActive()
    {
        return seal_lava_active.load();
    }
}