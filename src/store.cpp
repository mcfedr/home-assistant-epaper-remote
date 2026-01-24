#include "store.h"
#include "constants.h"
#include "esp_system.h"

static const char* TAG = "store";

void store_init(EntityStore* store) {
    store->mutex = xSemaphoreCreateMutex();
    store->event_group = xEventGroupCreate();
}

void store_set_wifi_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->wifi;
    store->wifi = state;
    xSemaphoreGive(store->mutex);

    if (state != previous_state) {
        if (state == ConnState::Up) {
            xEventGroupSetBits(store->event_group, BIT_WIFI_UP);
        } else {
            xEventGroupClearBits(store->event_group, BIT_WIFI_UP);
        }

        if (store->ui_task) {
            xTaskNotifyGive(store->ui_task);
        }
    }
}

void store_set_hass_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->home_assistant;
    store->home_assistant = state;
    xSemaphoreGive(store->mutex);

    if (state != previous_state && store->ui_task) {
        xTaskNotifyGive(store->ui_task);
    }
}

void store_update_value(EntityStore* store, uint8_t entity_idx, uint8_t value) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    HomeAssistantEntity& entity = store->entities[entity_idx];
    uint8_t previous_value = entity.current_value;
    entity.current_value = value;
    xSemaphoreGive(store->mutex);

    if (previous_value != value && store->ui_task) {
        xTaskNotifyGive(store->ui_task);
    }
}

void store_send_command(EntityStore* store, uint8_t entity_idx, uint8_t value) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    HomeAssistantEntity& entity = store->entities[entity_idx];
    entity.current_value = value;
    entity.command_value = value;
    entity.command_pending = true;
    xSemaphoreGive(store->mutex);

    ESP_LOGI(TAG, "Sending command to update entity %s to value %d", store->entities[entity_idx].entity_id, value);

    if (store->home_assistant_task) {
        xTaskNotifyGive(store->home_assistant_task);
    }
    if (store->ui_task) {
        xTaskNotifyGive(store->ui_task);
    }
}

bool store_get_pending_command(EntityStore* store, Command* command) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    for (uint8_t entity_idx = 0; entity_idx < store->entity_count; ++entity_idx) {
        HomeAssistantEntity& entity = store->entities[entity_idx];
        if (entity.command_pending) {
            command->entity_id = entity.entity_id;
            command->entity_idx = entity_idx;
            command->type = entity.command_type;
            command->value = entity.command_value;
            xSemaphoreGive(store->mutex);
            return true;
        }
    }

    xSemaphoreGive(store->mutex);
    return false;
}

void store_ack_pending_command(EntityStore* store, const Command* command) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    HomeAssistantEntity& entity = store->entities[command->entity_idx];
    if (entity.command_value == command->value) {
        entity.command_pending = false;
    }

    xSemaphoreGive(store->mutex);
}

void store_update_ui_state(EntityStore* store, const Screen* screen, UIState* ui_state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    // Handle wifi and home assistant state
    if (store->wifi == ConnState::Up && store->home_assistant == ConnState::Up) {
        ui_state->mode = UiMode::MainScreen;
    } else if (store->wifi == ConnState::Initializing) {
        ui_state->mode = UiMode::Boot;
    } else if (store->wifi == ConnState::InvalidCredentials) {
        // Should not happen
        ui_state->mode = UiMode::WifiDisconnected;
    } else if (store->wifi == ConnState::ConnectionError) {
        ui_state->mode = UiMode::WifiDisconnected;
    } else if (store->home_assistant == ConnState::Initializing) {
        ui_state->mode = UiMode::Boot;
    } else if (store->home_assistant == ConnState::InvalidCredentials) {
        ui_state->mode = UiMode::HassInvalidKey;
    } else if (store->home_assistant == ConnState::ConnectionError) {
        ui_state->mode = UiMode::HassDisconnected;
    } else {
        ui_state->mode = UiMode::GenericError;
    }

    for (uint8_t widget_idx = 0; widget_idx < screen->widget_count; widget_idx++) {
        uint8_t entity_id = screen->entity_ids[widget_idx];
        ui_state->widget_values[widget_idx] = store->entities[entity_id].current_value;
    }
    xSemaphoreGive(store->mutex);
}

void store_wait_for_wifi_up(EntityStore* store) {
    xEventGroupWaitBits(store->event_group, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
}

void store_flush_pending_commands(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    for (uint8_t entity_idx = 0; entity_idx < store->entity_count; ++entity_idx) {
        store->entities[entity_idx].command_pending = false;
    }
    xSemaphoreGive(store->mutex);
}

EntityRef store_add_entity(EntityStore* store, EntityConfig entity) {
    if (store->entity_count >= MAX_ENTITIES) {
        esp_system_abort("too many entities declared !");
    }

    uint8_t entity_id = store->entity_count++;
    store->entities[entity_id] = {
        .entity_id = entity.entity_id,
        .command_type = entity.command_type,
    };

    return EntityRef{.index = entity_id};
}