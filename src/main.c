#include "mgos.h"
#include "mgos_gpio.h"
#include "mgos_sys_config.h"
#include "mgos_mqtt.h"
#include "mgos_wifi.h"
#include "esp_wifi.h"
#include "frozen.h"
#include "mgos_rpc.h"
#include "mgos_dash.h"
#include "mgos_i2c.h"
#include "mgos_ds3231.h"

// Define the default I2C address for the DS3231 if not defined
#ifndef MGOS_DS3231_DEFAULT_I2C_ADDR
#define MGOS_DS3231_DEFAULT_I2C_ADDR 0x68
#endif

// Global variables
static mgos_timer_id report_timer_id;
static const char *rpc_topic_pub;
static const char *rpc_topic_sub;
static struct mgos_ds3231 *rtc = NULL;
static int output_pin = 2; // Define the output pin here (e.g., GPIO 2)

// Function to update a value in Mongoose OS configuration
static bool set_config_value(const char *key, const char *value, bool is_string) {
  char json[256];
  if (is_string) {
    snprintf(json, sizeof(json), "{\"%s\": \"%s\"}", key, value);
  } else {
    snprintf(json, sizeof(json), "{\"%s\": %s}", key, value);
  }

  LOG(LL_INFO, ("Applying config: %s", json));

  struct mg_str json_str = mg_mk_str(json);

  if (mgos_config_apply_s(json_str, false)) {
    if (mgos_sys_config_save(&mgos_sys_config, false, NULL)) {
      LOG(LL_INFO, ("Config saved successfully"));
      return true;
    } else {
      LOG(LL_ERROR, ("Failed to save config"));
      return false;
    }
  } else {
    LOG(LL_ERROR, ("Failed to apply config: %s", json));
    return false;
  }
}

// Function to save total bag count and machine state to JSON
static void save_state_to_json() {
  const char *filename = mgos_sys_config_get_coin_count_file();
  double total_bag = mgos_sys_config_get_app_total_bag();
  bool machine_on = mgos_gpio_read(output_pin);
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
  char *json_str = json_asprintf("{total_bag: %.2f, machine_on: %s, time: \"%s\"}", total_bag, machine_on ? "true" : "false", time_str);
  if (json_str != NULL) {
    FILE *f = fopen(filename, "w");
    if (f != NULL) {
      fwrite(json_str, 1, strlen(json_str), f);
      fclose(f);
      LOG(LL_INFO, ("State saved to JSON: total_bag=%.2f, machine_on=%s", total_bag, machine_on ? "true" : "false"));
    } else {
      LOG(LL_ERROR, ("Failed to open file for writing"));
    }
    free(json_str);
  } else {
    LOG(LL_ERROR, ("Failed to allocate memory for JSON string"));
  }
}

// Function to load total bag count and machine state from JSON
static void load_state_from_json() {
  const char *filename = mgos_sys_config_get_coin_count_file();
  FILE *f = fopen(filename, "r");
  if (f != NULL) {
    char buffer[256];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);
    double total_bag;
    bool machine_on;
    json_scanf(buffer, len, "{total_bag: %lf, machine_on: %B}", &total_bag, &machine_on);
    mgos_sys_config_set_app_total_bag(total_bag);
    mgos_gpio_write(output_pin, machine_on);
    LOG(LL_INFO, ("State loaded from JSON: total_bag=%.2f, machine_on=%s", total_bag, machine_on ? "true" : "false"));
  } else {
    LOG(LL_ERROR, ("Failed to open file for reading, starting from initial value"));
  }
}

// Function to toggle the pin and update the machine state
static void toggle_machine_state() {
  bool current_state = mgos_gpio_read(output_pin);
  bool new_state = !current_state;
  mgos_gpio_write(output_pin, new_state);

  // Save the new state to the configuration
  mgos_sys_config_set_app_machine_on(new_state);
  
  // Save the updated state to JSON
  save_state_to_json();

  // Create the JSON message to publish
  char message[256];
  snprintf(message, sizeof(message), "{\"id\": 1932, \"src\": \"client\", \"method\": \"Config.Set\", \"params\": {\"key\": \"app.machine_on\", \"value\": %s}}", new_state ? "true" : "false");
  
  // Publish the state to the MQTT topic
  mgos_mqtt_pub(rpc_topic_pub, message, strlen(message), 1, false);
  
  LOG(LL_INFO, ("Toggled machine state: %s", new_state ? "true" : "false"));
}

// ISR for coin insertion
static void coin_isr(int pin, void *arg) {
  double total_bag = mgos_sys_config_get_app_total_bag() + 1.0;
  mgos_sys_config_set_app_total_bag(total_bag);
  LOG(LL_INFO, ("Coin inserted! Total bag count: %.2f", total_bag));
  // Toggle the machine state on coin insertion
  toggle_machine_state();
  (void) pin;
  (void) arg;
}

// Timer callback to periodically save the total bag count
static void report_timer_cb(void *arg) {
  save_state_to_json();
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
  char message[256];
  snprintf(message, sizeof(message), "{\"total_bag\": %.2f, \"machine_on\": %s, \"time\": \"%s\"}", mgos_sys_config_get_app_total_bag(), mgos_gpio_read(output_pin) ? "true" : "false", time_str);
  mgos_mqtt_pub(rpc_topic_pub, message, strlen(message), 1, false);
  LOG(LL_INFO, ("Reported state: %s", message));
  (void) arg;
}

// RPC handler to change the configuration
static void rpc_set_message_handler(struct mg_rpc_request_info *ri,
                                    const char *args,
                                    const char *src,
                                    void *cb_arg) {
  char key[100];
  char value[100];
  bool is_string;

  LOG(LL_INFO, ("Received RPC call with args: %s", args));

  int scanned_items = json_scanf(args, strlen(args), "{key: %99[^,], value: %99[^}]}", key, value);
  LOG(LL_INFO, ("Scanned items: %d", scanned_items));
  LOG(LL_INFO, ("Parsed key: %s, value: %s", key, value));

  if (scanned_items == 2) {
    is_string = !(strspn(value, "0123456789.-") == strlen(value));
    if (set_config_value(key, value, is_string)) {
      mg_rpc_send_responsef(ri, "{id: %d, result: true}", ri->id);
      LOG(LL_INFO, ("Updated config key %s to value %s via RPC", key, value));
      if (strcmp(key, "app.machine_on") == 0) {
        mgos_gpio_write(output_pin, strcmp(value, "true") == 0);
        save_state_to_json();
      }
    } else {
      mg_rpc_send_errorf(ri, 400, "Failed to set value for key: %s", key);
      LOG(LL_ERROR, ("Failed to update config key %s via RPC", key));
    }
  } else {
    mg_rpc_send_errorf(ri, 400, "Invalid parameters format");
    LOG(LL_ERROR, ("Invalid parameters format in RPC request"));
  }

  (void) cb_arg;
}

// RPC handler to get the JSON content
static void rpc_get_counters_handler(struct mg_rpc_request_info *ri,
                                     const char *args,
                                     const char *src,
                                     void *cb_arg) {
  const char *filename = mgos_sys_config_get_coin_count_file();
  FILE *f = fopen(filename, "r");
  if (f != NULL) {
    char buffer[256];
    int len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);
    mg_rpc_send_responsef(ri, "%s", buffer);
    LOG(LL_INFO, ("Sent JSON content: %s", buffer));
  } else {
    mg_rpc_send_errorf(ri, 500, "Failed to open file for reading");
    LOG(LL_ERROR, ("Failed to open file for reading"));
  }
  (void) args;
  (void) src;
  (void) cb_arg;
}

// MQTT message handler to change the configuration
static void mqtt_message_handler(struct mg_connection *nc, const char *topic,
                                 int topic_len, const char *msg, int msg_len,
                                 void *userdata) {
  LOG(LL_INFO, ("Received message on topic %.*s: %.*s", topic_len, topic, msg_len, msg));

  struct json_token method_token, params_token, key_token, value_token;
  if (json_scanf(msg, msg_len, "{method: %T, params: %T}", &method_token, &params_token) == 2) {
    if (strncmp(method_token.ptr, "Config.Set", method_token.len) == 0) {
      if (json_scanf(params_token.ptr, params_token.len, "{key: %T, value: %T}", &key_token, &value_token) == 2) {
        char key[100], value[100];
        snprintf(key, sizeof(key), "%.*s", key_token.len, key_token.ptr);
        snprintf(value, sizeof(value), "%.*s", value_token.len, value_token.ptr);

        bool is_string = !(strspn(value, "0123456789.-") == strlen(value));
        if (set_config_value(key, value, is_string)) {
          LOG(LL_INFO, ("Updated config key %s to value %s via MQTT", key, value));
          if (strcmp(key, "app.machine_on") == 0) {
            mgos_gpio_write(output_pin, strcmp(value, "true") == 0);
            save_state_to_json();
          }
        } else {
          LOG(LL_ERROR, ("Failed to update config key %s via MQTT", key));
        }
      } else {
        LOG(LL_ERROR, ("Invalid JSON format for key and value parameters"));
      }
    }
  } else {
    LOG(LL_ERROR, ("Invalid JSON format"));
  }

  (void) nc;
  (void) userdata;
}

// Function to initialize the DS3231
bool ds3231_init(void) {
  rtc = mgos_ds3231_create(MGOS_DS3231_DEFAULT_I2C_ADDR);
  if (rtc == NULL) {
    LOG(LL_ERROR, ("Failed to initialize DS3231"));
    return false;
  } else {
    LOG(LL_INFO, ("DS3231 initialized"));
    return true;
  }
}

enum mgos_app_init_result mgos_app_init(void) {
  load_state_from_json(); // Load the initial total bag count and machine state from JSON

  int coin_pin = mgos_sys_config_get_coin_pin();
  mgos_gpio_set_mode(coin_pin, MGOS_GPIO_MODE_INPUT);
  mgos_gpio_set_pull(coin_pin, MGOS_GPIO_PULL_UP);
  mgos_gpio_set_int_handler(coin_pin, MGOS_GPIO_INT_EDGE_NEG, coin_isr, NULL);
  mgos_gpio_enable_int(coin_pin);
  
  int report_delay = mgos_sys_config_get_coin_report_delay();
  report_timer_id = mgos_set_timer(report_delay, MGOS_TIMER_REPEAT, report_timer_cb, NULL);

  rpc_topic_pub = mgos_sys_config_get_rpc_mqtt_pub_topic();
  rpc_topic_sub = mgos_sys_config_get_rpc_mqtt_sub_topic();

  // Register the RPC handlers
  mgos_rpc_add_handler("Config.Set", rpc_set_message_handler, NULL);
  mgos_rpc_add_handler("Counters.Get", rpc_get_counters_handler, NULL);
  LOG(LL_INFO, ("RPC handlers registered successfully"));

  // WiFi configuration
  const char *ssid = mgos_sys_config_get_wifi_sta_ssid();
  const char *pass = mgos_sys_config_get_wifi_sta_pass();
  if (ssid && pass) {
    struct mgos_config_wifi_sta sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    sta_cfg.enable = true;
    sta_cfg.ssid = strdup(ssid);
    sta_cfg.pass = strdup(pass);
    mgos_wifi_setup_sta(&sta_cfg);
    free((void *) sta_cfg.ssid);
    free((void *) sta_cfg.pass);
  } else {
    LOG(LL_ERROR, ("WiFi SSID or password not set"));
    return MGOS_APP_INIT_ERROR;
  }

  // Initialize DS3231
  if (!ds3231_init()) {
    return MGOS_APP_INIT_ERROR;
  }

  // Initialize the output pin
  mgos_gpio_set_mode(output_pin, MGOS_GPIO_MODE_OUTPUT);
  mgos_gpio_write(output_pin, 0); // Initialize to low

  return MGOS_APP_INIT_SUCCESS;
}
