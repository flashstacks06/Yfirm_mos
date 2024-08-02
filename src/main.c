#include "mgos.h"
#include "mgos_gpio.h"
#include "mgos_sys_config.h"
#include "frozen.h"
#include <stdio.h>

// JSON file to save machine state
static const char *state_filename = "machine_state.json";

// Variable to keep track of the previous state
static bool prev_machine_on = false;

void save_machine_state(bool state) {
  char *json_str = json_asprintf("{\"machine_on\": %s}", state ? "true" : "false");
  if (json_str != NULL) {
    FILE *f = fopen(state_filename, "w");
    if (f != NULL) {
      fwrite(json_str, 1, strlen(json_str), f);
      fclose(f);
      LOG(LL_INFO, ("Saved machine state to file: %s", state_filename));
    } else {
      LOG(LL_ERROR, ("Failed to open file for writing"));
    }
    free(json_str);
  } else {
    LOG(LL_ERROR, ("Failed to allocate memory for JSON string"));
  }
}

void load_machine_state() {
  FILE *f = fopen(state_filename, "r");
  if (f != NULL) {
    char buffer[100];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    if (len > 0) {
      buffer[len] = '\0';
      struct json_token t;
      if (json_scanf(buffer, len, "{machine_on: %B}", &t) == 1) {
        prev_machine_on = t.ptr[0] == 't';
      }
    }
    fclose(f);
  }
}

void update_relay_pin(bool state) {
  int pin_relay = mgos_sys_config_get_app_pin_relay();
  mgos_gpio_set_mode(pin_relay, MGOS_GPIO_MODE_OUTPUT);
  mgos_gpio_write(pin_relay, state ? 0 : 1);
}

bool is_within_time_range(struct tm *current_time) {
  int start_hour = mgos_sys_config_get_app_start_hour();
  int start_minute = mgos_sys_config_get_app_start_minute();
  int end_hour = mgos_sys_config_get_app_end_hour();
  int end_minute = mgos_sys_config_get_app_end_minute();

  int current_hour = current_time->tm_hour;
  int current_minute = current_time->tm_min;

  if (start_hour < end_hour || (start_hour == end_hour && start_minute <= end_minute)) {
    return (current_hour > start_hour || (current_hour == start_hour && current_minute >= start_minute)) &&
           (current_hour < end_hour || (current_hour == end_hour && current_minute <= end_minute));
  } else {
    return (current_hour > start_hour || (current_hour == start_hour && current_minute >= start_minute)) ||
           (current_hour < end_hour || (current_hour == end_hour && current_minute <= end_minute));
  }
}

void check_machine_state(void *arg) {
  int pin_machine = mgos_sys_config_get_app_pin_machine();
  bool machine_state = mgos_gpio_read(pin_machine);

  bool machine_on = (machine_state == 1); // If pin_machine is 1, set machine_on to true; otherwise, false

  // Get the current time
  time_t now = time(NULL);
  struct tm *current_time = localtime(&now);

  // Update machine_on based on the time range
  bool within_time_range = is_within_time_range(current_time);
  machine_on = within_time_range ? true : false;

  // Update and save only if there is a change in state
  if (machine_on != prev_machine_on) {
    mgos_sys_config_set_app_machine_on(machine_on);
    save_machine_state(machine_on);
    prev_machine_on = machine_on; // Update the previous state
    update_relay_pin(machine_on);

    LOG(LL_INFO, ("Machine state: %d, Machine on: %s", machine_state, machine_on ? "true" : "false"));
  }

  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  int pin_machine = mgos_sys_config_get_app_pin_machine();
  mgos_gpio_set_mode(pin_machine, MGOS_GPIO_MODE_INPUT);

  // Load the previous state from file
  load_machine_state();

  // Initialize relay pin based on the previous state
  update_relay_pin(prev_machine_on);

  mgos_set_timer(mgos_sys_config_get_app_check_interval_ms(), MGOS_TIMER_REPEAT, check_machine_state, NULL);

  return MGOS_APP_INIT_SUCCESS;
}
