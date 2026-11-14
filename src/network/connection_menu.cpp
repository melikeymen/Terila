#include "network/connection.hpp"

#include "protocol/menu.hpp"

#include <algorithm>
#include <array>

namespace terila::network {

    namespace {

        // Menu registry ids from input/misc/menus.json.
        constexpr std::int32_t kMenuTypeCrafting = 12;
        constexpr std::int32_t kMenuTypeFurnace = 14;

        // Slot counts of both windows: result/grid or input/fuel/output, then the player
        // inventory and the hotbar (36 slots, contiguous).
        constexpr std::size_t kCraftingSlots = 46;
        constexpr std::size_t kFurnaceSlots = 39;

        // Two stacks only merge when the item and its data components match; otherwise
        // an enchanted item would silently absorb a plain one (and vice versa).
        bool same_stack(std::int32_t item, const terila::item::ComponentsPtr& left,
            std::int32_t other_item, const terila::item::ComponentsPtr& right) {
            return item == other_item && terila::item::same_components(left, right);
        }

    }

    int Connection::menu_kind_for_state(std::int32_t state) const {
        const auto it = menu_blocks_.find(state);
        return it == menu_blocks_.end() ? kMenuNone : static_cast<int>(it->second);
    }

    void Connection::send_menu_content() {
        if (!menu_open_) {
            return;
        }

        std::vector<terila::protocol::MenuItem> items;
        items.reserve(menu_slots_.size());
        for (const auto& slot : menu_slots_) {
            items.push_back(terila::protocol::MenuItem{ slot.item_id, slot.count,
                slot.components.get() });
        }

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_container_set_content(out, menu_id_, menu_state_id_, items,
                terila::protocol::MenuItem{ cursor_.item_id, cursor_.count,
                    cursor_.components.get() },
                &component_ids_)) {
            send(out);
        }
    }

    void Connection::send_menu_slot(std::int32_t slot) {
        if (!menu_open_ || slot < 0 || slot >= static_cast<std::int32_t>(menu_slots_.size())) {
            return;
        }

        const auto& entry = menu_slots_[static_cast<std::size_t>(slot)];
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_container_set_slot(out,
                static_cast<std::uint8_t>(menu_id_), menu_state_id_,
                static_cast<std::int16_t>(slot), entry.item_id, entry.count,
                entry.components.get(), &component_ids_)) {
            send(out);
        }
    }

    void Connection::open_menu(int kind, std::string_view title) {
        if (recipes_ == nullptr || kind == kMenuNone) {
            return;
        }

        // Opening another window discards the current one's layout, so its items and the
        // cursor go back to the inventory first instead of vanishing.
        if (menu_open_) {
            close_menu();
        }

        menu_kind_ = static_cast<MenuKind>(kind);
        menu_open_ = true;
        ++menu_id_;
        ++menu_state_id_;

        menu_slots_.assign(kind == kMenuCrafting ? kCraftingSlots : kFurnaceSlots, MenuSlot{});
        const std::int32_t offset = menu_player_offset();
        for (std::int32_t i = 0; i < kInventorySlots; ++i) {
            const auto& entry = inventory_[static_cast<std::size_t>(i)];
            menu_slots_[static_cast<std::size_t>(offset + i)] =
                MenuSlot{ entry.item_id, entry.count, entry.components };
        }

        cursor_ = MenuSlot{};
        furnace_burn_ = 0;
        furnace_burn_total_ = 0;
        furnace_cook_ = 0;
        last_burn_ = -1;
        last_burn_total_ = -1;
        last_cook_ = -1;
        last_cook_total_ = -1;

        std::vector<std::uint8_t> out;
        const std::int32_t type = kind == kMenuCrafting ? kMenuTypeCrafting : kMenuTypeFurnace;
        if (terila::protocol::write_open_screen(out, menu_id_, type, title)) {
            send(out);
        }

        recompute_crafting_result();
        send_menu_content();
        LOG_DEBUG(Network, "{} opened the {} menu", username_,
            kind == kMenuCrafting ? "crafting" : "furnace");
    }

    void Connection::close_menu() {
        if (!menu_open_) {
            return;
        }

        const std::int32_t offset = menu_player_offset();
        for (std::int32_t i = 0; i < kInventorySlots; ++i) {
            const auto& entry = menu_slots_[static_cast<std::size_t>(offset + i)];
            inventory_[static_cast<std::size_t>(i)] =
                InvSlot{ entry.item_id, entry.count, entry.components };
        }

        menu_open_ = false;
        menu_kind_ = kMenuNone;
        menu_slots_.clear();

        const MenuSlot pending = cursor_;
        cursor_ = MenuSlot{};
        if (pending.item_id != 0 && pending.count > 0) {
            if (!add_to_inventory(pending.item_id, pending.count, pending.components)) {
                LOG_DEBUG(Network, "{}: cursor item {} x{} did not fit in inventory",
                    username_, pending.item_id, pending.count);
            }
        }

        for (std::int32_t i = 0; i < kInventorySlots; ++i) {
            send_inventory_slot(kFirstInventorySlot + i);
        }

        refresh_held_item();
    }

    void Connection::handle_container_close(std::int32_t window_id) {
        if (!menu_open_ || window_id != menu_id_) {
            return;
        }
        close_menu();
    }

    void Connection::recompute_crafting_result() {
        if (!menu_open_ || menu_kind_ != kMenuCrafting || recipes_ == nullptr) {
            return;
        }

        std::array<std::int32_t, 9> grid{};
        for (std::size_t i = 0; i < grid.size(); ++i) {
            grid[i] = menu_slots_[1 + i].item_id;
        }

        const auto recipe = recipes_->match_crafting(grid);
        menu_slots_[0] = recipe ? MenuSlot{ recipe->item_id, recipe->count } : MenuSlot{};
    }

    void Connection::handle_container_click(const std::vector<std::uint8_t>& payload) {
        if (!menu_open_) {
            return;
        }

        const auto click = terila::protocol::read_container_click(payload.data(), payload.size());
        if (!click || click->window_id != menu_id_) {
            return;
        }
        // -999 marks the outside of the window, which drag clicks use as start and end.
        const bool outside = click->slot == -999;
        if (!outside && (click->slot < 0 ||
                click->slot >= static_cast<std::int32_t>(menu_slots_.size()))) {
            return;
        }

        if (click->click_type == 3 && !outside) {
            // Creative middle click: copy a full stack onto the empty cursor.
            clone_slot(click->slot);
            return;
        }
        if (click->click_type == 5) {
            drag_click(click->button, click->slot, outside);
            return;
        }

        if (click->click_type == 1 && !outside) {
            quick_move(click->slot);
            recompute_crafting_result();
            send_menu_content();
            return;
        }

        // Pickup clicks (type 0) are interpreted; everything else is ignored and the
        // client is resynced from the authoritative contents.
        if (click->click_type == 0 && !outside) {
            if (menu_kind_ == kMenuCrafting && click->slot == 0) {
                if (menu_slots_[0].count > 0) {
                    const auto result = menu_slots_[0];
                    const bool compatible = cursor_.count == 0 ||
                        (same_stack(cursor_.item_id, cursor_.components, result.item_id,
                             result.components) &&
                         cursor_.count + result.count <= kMaxStackSize);
                    if (compatible) {
                        cursor_ = MenuSlot{ result.item_id, cursor_.count + result.count,
                            cursor_.components };
                        for (std::size_t i = 1; i <= 9; ++i) {
                            if (menu_slots_[i].count > 0 && --menu_slots_[i].count == 0) {
                                menu_slots_[i].item_id = 0;
                            }
                        }
                    }
                }
            }
            else {
                auto& target = menu_slots_[static_cast<std::size_t>(click->slot)];
                const int button = click->button;

                if (button == 1) {
                    if (cursor_.count == 0) {
                        if (target.count > 0) {
                            const std::int32_t take = (target.count + 1) / 2;
                            cursor_ = MenuSlot{ target.item_id, take, target.components };
                            target.count -= take;
                            if (target.count == 0) {
                                target.item_id = 0;
                            }
                        }
                    }
                    else if (target.count == 0) {
                        target = MenuSlot{ cursor_.item_id, 1, cursor_.components };
                        if (--cursor_.count == 0) {
                            cursor_.item_id = 0;
                        }
                    }
                    else if (target.count < kMaxStackSize &&
                        same_stack(target.item_id, target.components, cursor_.item_id,
                            cursor_.components)) {
                        ++target.count;
                        if (--cursor_.count == 0) {
                            cursor_.item_id = 0;
                        }
                    }
                }
                else if (cursor_.count == 0) {
                    if (target.count > 0) {
                        cursor_ = target;
                        target = MenuSlot{};
                    }
                }
                else if (target.count == 0) {
                    target = cursor_;
                    cursor_ = MenuSlot{};
                }
                else if (same_stack(target.item_id, target.components, cursor_.item_id,
                    cursor_.components)) {
                    const std::int32_t moved = std::min(kMaxStackSize - target.count, cursor_.count);
                    target.count += moved;
                    cursor_.count -= moved;
                    if (cursor_.count == 0) {
                        cursor_.item_id = 0;
                    }
                }
                else {
                    const MenuSlot held = cursor_;
                    cursor_ = target;
                    target = held;
                }
            }
        }

        recompute_crafting_result();
        send_menu_content();
    }

    // Creative middle click: the cursor takes a full stack of the clicked slot.
    void Connection::clone_slot(std::int32_t slot) {
        if (gamemode_ != 1 || cursor_.count > 0) {
            return;
        }
        const auto& target = menu_slots_[static_cast<std::size_t>(slot)];
        if (target.count <= 0) {
            return;
        }

        cursor_ = MenuSlot{ target.item_id, kMaxStackSize, target.components };
        recompute_crafting_result();
        send_menu_content();
    }

    bool Connection::place_one_from_cursor(std::int32_t slot) {
        if (cursor_.count <= 0) {
            return false;
        }
        if (menu_kind_ == kMenuCrafting && slot == 0) {
            return false;
        }

        auto& target = menu_slots_[static_cast<std::size_t>(slot)];
        if (target.count == 0) {
            target = MenuSlot{ cursor_.item_id, 1, cursor_.components };
        }
        else if (target.count < kMaxStackSize &&
            same_stack(target.item_id, target.components, cursor_.item_id,
                cursor_.components)) {
            ++target.count;
        }
        else {
            return false;
        }

        if (--cursor_.count == 0) {
            cursor_.item_id = 0;
        }
        return true;
    }

    // Quick craft drag: the client sends one click per slot the mouse crosses, each of
    // which drops a single item from the cursor into that slot.
    void Connection::drag_click(std::int32_t button, std::int32_t slot, bool outside) {
        const int phase = button >> 2;
        const int mode = button & 3;

        if (phase == 0) {
            dragging_ = true;
            drag_mode_ = mode;
            drag_slots_.clear();
            return;
        }

        if (phase == 2 || outside) {
            dragging_ = false;
            drag_slots_.clear();
            recompute_crafting_result();
            send_menu_content();
            return;
        }

        if (!dragging_) {
            return;
        }

        // Each slot only receives an item once per drag.
        if (std::find(drag_slots_.begin(), drag_slots_.end(), slot) != drag_slots_.end()) {
            return;
        }
        drag_slots_.push_back(slot);
        place_one_from_cursor(slot);

        recompute_crafting_result();
        send_menu_content();
    }

    // Room left for one item id across the player slots of the open window.
    std::int32_t Connection::menu_player_room(std::int32_t item_id,
        const terila::item::ComponentsPtr& components) const {
        const std::int32_t offset = menu_player_offset();
        std::int32_t room = 0;

        for (std::int32_t i = offset; i < offset + kInventorySlots; ++i) {
            const auto& target = menu_slots_[static_cast<std::size_t>(i)];
            if (target.count == 0) {
                room += kMaxStackSize;
            }
            else if (same_stack(target.item_id, target.components, item_id, components)) {
                room += kMaxStackSize - target.count;
            }
        }
        return room;
    }

    std::int32_t Connection::add_to_menu_player(std::int32_t item_id, std::int32_t count,
        const terila::item::ComponentsPtr& components) {
        const std::int32_t offset = menu_player_offset();
        std::int32_t remaining = count;

        for (int pass = 0; pass < 2 && remaining > 0; ++pass) {
            for (std::int32_t i = offset; i < offset + kInventorySlots && remaining > 0; ++i) {
                auto& target = menu_slots_[static_cast<std::size_t>(i)];

                if (pass == 0) {
                    if (target.item_id != item_id || target.count >= kMaxStackSize ||
                        !terila::item::same_components(target.components, components)) {
                        continue;
                    }
                    const std::int32_t added = std::min(kMaxStackSize - target.count, remaining);
                    target.count += added;
                    remaining -= added;
                }
                else {
                    if (target.count > 0) {
                        continue;
                    }
                    const std::int32_t added = std::min(kMaxStackSize, remaining);
                    target.item_id = item_id;
                    target.count = added;
                    target.components = components;
                    remaining -= added;
                }
            }
        }
        return remaining;
    }

    // Shift click: moves stacks between the window and the player inventory. In the
    // furnace fuel goes to the fuel slot and smeltables to the input slot; the crafting
    // result crafts as often as the inventory has room for.
    void Connection::quick_move(std::int32_t slot) {
        const std::int32_t offset = menu_player_offset();

        if (slot >= offset) {
            auto& source = menu_slots_[static_cast<std::size_t>(slot)];
            if (source.count <= 0 || menu_kind_ != kMenuFurnace) {
                return;
            }

            std::int32_t dest = -1;
            if (recipes_ != nullptr && recipes_->fuel_ticks(source.item_id) > 0) {
                dest = 1;
            }
            else if (recipes_ != nullptr && recipes_->find_cooking(source.item_id)) {
                dest = 0;
            }
            if (dest < 0) {
                return;
            }

            auto& target = menu_slots_[static_cast<std::size_t>(dest)];
            if (target.count > 0 && !same_stack(target.item_id, target.components,
                    source.item_id, source.components)) {
                return;
            }
            const std::int32_t room = kMaxStackSize - target.count;
            if (room <= 0) {
                return;
            }

            const std::int32_t moved = std::min(room, source.count);
            target.item_id = source.item_id;
            target.components = source.components;
            target.count += moved;
            source.count -= moved;
            if (source.count == 0) {
                source.item_id = 0;
            }
            return;
        }

        if (menu_kind_ == kMenuCrafting && slot == 0) {
            while (menu_slots_[0].item_id != 0) {
                const std::int32_t result_item = menu_slots_[0].item_id;
                const std::int32_t result_count = menu_slots_[0].count;
                if (menu_player_room(result_item, nullptr) < result_count) {
                    break;
                }
                add_to_menu_player(result_item, result_count);
                for (std::size_t i = 1; i <= 9; ++i) {
                    if (menu_slots_[i].count > 0 && --menu_slots_[i].count == 0) {
                        menu_slots_[i].item_id = 0;
                    }
                }
                recompute_crafting_result();
            }
            return;
        }

        auto& source = menu_slots_[static_cast<std::size_t>(slot)];
        if (source.count <= 0) {
            return;
        }

        const std::int32_t remaining = add_to_menu_player(source.item_id, source.count,
            source.components);
        source.count = remaining;
        if (remaining == 0) {
            source.item_id = 0;
        }
    }

    void Connection::send_furnace_data(std::int32_t data_id, std::int32_t value) {
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_container_set_data(out, menu_id_, data_id, value)) {
            send(out);
        }
    }

    // Furnaces burn fuel while a smeltable item sits in the input slot and push the result
    // into the output slot once the cook time elapses.
    void Connection::tick_menu() {
        if (!menu_open_ || menu_kind_ != kMenuFurnace || recipes_ == nullptr) {
            return;
        }

        auto& input = menu_slots_[0];
        auto& fuel = menu_slots_[1];
        auto& output = menu_slots_[2];

        const auto recipe = recipes_->find_cooking(input.item_id);
        const bool can_cook = recipe.has_value() && input.count > 0 &&
            (output.count == 0 ||
                (output.item_id == recipe->result_item &&
                 output.count + recipe->result_count <= kMaxStackSize));

        if (furnace_burn_ > 0) {
            --furnace_burn_;
        }

        if (furnace_burn_ == 0 && can_cook) {
            const int ticks = recipes_->fuel_ticks(fuel.item_id);
            if (ticks > 0) {
                furnace_burn_ = furnace_burn_total_ = ticks;
                if (--fuel.count == 0) {
                    fuel.item_id = 0;
                }
                send_menu_slot(1);
            }
        }

        if (can_cook && furnace_burn_ > 0) {
            if (++furnace_cook_ >= recipe->cook_ticks) {
                furnace_cook_ = 0;
                if (--input.count == 0) {
                    input.item_id = 0;
                }
                output.item_id = recipe->result_item;
                output.count += recipe->result_count;
                send_menu_slot(0);
                send_menu_slot(2);
            }
        }
        else {
            furnace_cook_ = 0;
        }

        // 0 = burn time left, 1 = burn time total, 2 = cook progress, 3 = cook total.
        // The client draws its flame and progress arrow straight from these.
        const std::int32_t cook_total = recipe ? recipe->cook_ticks : 0;
        if (furnace_burn_ != last_burn_) {
            send_furnace_data(0, furnace_burn_);
            last_burn_ = furnace_burn_;
        }
        if (furnace_burn_total_ != last_burn_total_) {
            send_furnace_data(1, furnace_burn_total_);
            last_burn_total_ = furnace_burn_total_;
        }
        if (furnace_cook_ != last_cook_) {
            send_furnace_data(2, furnace_cook_);
            last_cook_ = furnace_cook_;
        }
        if (cook_total != last_cook_total_) {
            send_furnace_data(3, cook_total);
            last_cook_total_ = cook_total;
        }
    }

} // namespace terila::network
