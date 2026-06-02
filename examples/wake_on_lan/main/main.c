/**
 * @file main.c
 * @brief MicroLink v2 Tailscale Wake-on-LAN Proxy Example
 *
 * Connects to a Tailscale network and:
 * - Hosts a glassmorphic dashboard on http://<IP>/ (both Local & Tailscale VPN IP)
 * - Exposes GET /api/status for real-time ESP32/VPN statistics
 * - Exposes GET /api/wake?mac=xx:xx:xx:xx:xx:xx to trigger WOL broadcasts
 * - Listens on Tailscale UDP port 9000 to trigger WOL via UDP payload
 * - Emulates WLED JSON API for HyperHDR compatibility (/json, /json/state, etc.)
 * - Supports DDP (Distributed Display Protocol) on port 4048 for pixel streaming
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#if CONFIG_WOL_LED_STRIP_ENABLED
#include "led_strip.h"
#endif

#include "microlink.h"
#include "microlink_internal.h"
#include "ml_config_httpd.h"

static const char *TAG = "wol_proxy";

/* WiFi credentials — start with Kconfig defaults, NVS may override */
static char wifi_ssid[33]     = CONFIG_ML_WIFI_SSID;
static char wifi_password[65] = CONFIG_ML_WIFI_PASSWORD;

/* Multi-SSID support */
static ml_config_wifi_list_t wifi_list;
static int wifi_list_count = 0;      /* 0 = single SSID mode */
static int current_wifi_idx = 0;
static int wifi_retry_count = 0;
#define WIFI_MAX_RETRIES_PER_SSID 3

/* WiFi event group */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* MicroLink handles */
static microlink_t *ml = NULL;
static microlink_udp_socket_t *udp_sock = NULL;

/* HTTP Server handle */
static httpd_handle_t server = NULL;

/* Global state variables for Web UI */
static uint32_t wifi_ip_addr = 0;
static char wifi_ip_str[16] = "0.0.0.0";
static char vpn_ip_str[16] = "0.0.0.0";

#if CONFIG_WOL_LED_STRIP_ENABLED
/* WLED emulation state */
static bool wled_on = true;
static uint8_t wled_brightness = 255;
static char esp_mac_str[13] = "000000000000"; /* hex MAC for WLED info */
#endif

/* HTML Dashboard content (glassmorphic dark theme) */
static const char html_page[] = 
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"  <meta charset='UTF-8'>\n"
"  <meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"  <title>MicroLink WOL Dashboard</title>\n"
"  <link rel='preconnect' href='https://fonts.googleapis.com'>\n"
"  <link rel='preconnect' href='https://fonts.gstatic.com' crossorigin>\n"
"  <link href='https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700&display=swap' rel='stylesheet'>\n"
"  <style>\n"
"    :root {\n"
"      --bg: radial-gradient(circle at top right, #110e26, #06050b);\n"
"      --card-bg: rgba(255, 255, 255, 0.02);\n"
"      --card-border: rgba(255, 255, 255, 0.08);\n"
"      --text: #f3f4f6;\n"
"      --text-dim: #9ca3af;\n"
"      --gradient: linear-gradient(135deg, #7928ca, #ff0080);\n"
"      --green: #10b981;\n"
"      --green-bg: rgba(16, 185, 129, 0.1);\n"
"      --shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.5);\n"
"    }\n"
"    * { box-sizing: border-box; }\n"
"    body {\n"
"      margin: 0;\n"
"      padding: 0;\n"
"      background: var(--bg);\n"
"      color: var(--text);\n"
"      font-family: 'Outfit', sans-serif;\n"
"      min-height: 100vh;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      align-items: center;\n"
"      justify-content: flex-start;\n"
"      overflow-x: hidden;\n"
"    }\n"
"    .wrapper {\n"
"      width: 90%;\n"
"      max-width: 800px;\n"
"      margin: 40px auto;\n"
"    }\n"
"    .header {\n"
"      display: flex;\n"
"      align-items: center;\n"
"      justify-content: space-between;\n"
"      margin-bottom: 30px;\n"
"      padding: 20px;\n"
"      background: var(--card-bg);\n"
"      border: 1px solid var(--card-border);\n"
"      border-radius: 20px;\n"
"      backdrop-filter: blur(12px);\n"
"      -webkit-backdrop-filter: blur(12px);\n"
"      box-shadow: var(--shadow);\n"
"    }\n"
"    .brand {\n"
"      display: flex;\n"
"      align-items: center;\n"
"      gap: 12px;\n"
"    }\n"
"    .brand-logo {\n"
"      font-size: 24px;\n"
"      background: var(--gradient);\n"
"      -webkit-background-clip: text;\n"
"      -webkit-text-fill-color: transparent;\n"
"      font-weight: 700;\n"
"      letter-spacing: -0.5px;\n"
"    }\n"
"    .status-badge {\n"
"      display: flex;\n"
"      align-items: center;\n"
"      gap: 8px;\n"
"      padding: 6px 12px;\n"
"      background: rgba(255, 255, 255, 0.05);\n"
"      border: 1px solid var(--card-border);\n"
"      border-radius: 30px;\n"
"      font-size: 13px;\n"
"      font-weight: 500;\n"
"    }\n"
"    .status-dot {\n"
"      width: 8px;\n"
"      height: 8px;\n"
"      border-radius: 50%;\n"
"      background: #ef4444;\n"
"      box-shadow: 0 0 8px #ef4444;\n"
"    }\n"
"    .status-dot.connected {\n"
"      background: var(--green);\n"
"      box-shadow: 0 0 8px var(--green);\n"
"    }\n"
"    .grid {\n"
"      display: grid;\n"
"      grid-template-columns: 1fr;\n"
"      gap: 20px;\n"
"    }\n"
"    @media (min-width: 768px) {\n"
"      .grid { grid-template-columns: 1fr 1fr; }\n"
"      .full-span { grid-column: span 2; }\n"
"    }\n"
"    .card {\n"
"      background: var(--card-bg);\n"
"      border: 1px solid var(--card-border);\n"
"      border-radius: 20px;\n"
"      padding: 24px;\n"
"      backdrop-filter: blur(12px);\n"
"      -webkit-backdrop-filter: blur(12px);\n"
"      box-shadow: var(--shadow);\n"
"      display: flex; \n"
"      flex-direction: column;\n"
"      transition: all 0.3s cubic-bezier(0.16, 1, 0.3, 1);\n"
"    }\n"
"    .card:hover {\n"
"      transform: translateY(-2px);\n"
"      border-color: rgba(255, 255, 255, 0.15);\n"
"    }\n"
"    .card-title {\n"
"      font-size: 16px;\n"
"      font-weight: 600;\n"
"      color: var(--text-dim);\n"
"      margin-top: 0;\n"
"      margin-bottom: 20px;\n"
"      text-transform: uppercase;\n"
"      letter-spacing: 1px;\n"
"    }\n"
"    .metrics {\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 14px;\n"
"    }\n"
"    .metric {\n"
"      display: flex;\n"
"      justify-content: space-between;\n"
"      align-items: center;\n"
"      padding-bottom: 10px;\n"
"      border-bottom: 1px solid rgba(255, 255, 255, 0.04);\n"
"    }\n"
"    .metric:last-child {\n"
"      border-bottom: none;\n"
"      padding-bottom: 0;\n"
"    }\n"
"    .metric-label {\n"
"      font-size: 14px;\n"
"      color: var(--text-dim);\n"
"    }\n"
"    .metric-value {\n"
"      font-size: 15px;\n"
"      font-weight: 500;\n"
"      font-family: monospace;\n"
"    }\n"
"    .form-group {\n"
"      margin-bottom: 16px;\n"
"    }\n"
"    .form-group label {\n"
"      display: block;\n"
"      font-size: 13px;\n"
"      color: var(--text-dim);\n"
"      margin-bottom: 8px;\n"
"    }\n"
"    .input-field {\n"
"      width: 100%;\n"
"      background: rgba(0, 0, 0, 0.2);\n"
"      border: 1px solid var(--card-border);\n"
"      border-radius: 10px;\n"
"      padding: 12px;\n"
"      color: var(--text);\n"
"      font-family: monospace;\n"
"      font-size: 16px;\n"
"      transition: border-color 0.2s;\n"
"    }\n"
"    .input-field:focus {\n"
"      outline: none;\n"
"      border-color: #7928ca;\n"
"    }\n"
"    .btn-wake {\n"
"      background: var(--gradient);\n"
"      border: none;\n"
"      border-radius: 12px;\n"
"      color: white;\n"
"      padding: 14px;\n"
"      font-size: 16px;\n"
"      font-weight: 600;\n"
"      cursor: pointer;\n"
"      box-shadow: 0 4px 15px rgba(255, 0, 128, 0.2);\n"
"      transition: all 0.2s;\n"
"      display: flex;\n"
"      align-items: center;\n"
"      justify-content: center;\n"
"      gap: 8px;\n"
"    }\n"
"    .btn-wake:hover {\n"
"      box-shadow: 0 6px 20px rgba(255, 0, 128, 0.4);\n"
"      transform: translateY(-1px);\n"
"    }\n"
"    .btn-wake:active {\n"
"      transform: translateY(1px);\n"
"    }\n"
"    .btn-wake:disabled {\n"
"      background: #4b5563;\n"
"      box-shadow: none;\n"
"      cursor: not-allowed;\n"
"    }\n"
"    .history-log {\n"
"      max-height: 180px;\n"
"      overflow-y: auto;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 10px;\n"
"    }\n"
"    .history-item {\n"
"      display: flex;\n"
"      justify-content: space-between;\n"
"      align-items: center;\n"
"      padding: 8px 12px;\n"
"      background: rgba(255, 255, 255, 0.02);\n"
"      border: 1px solid rgba(255, 255, 255, 0.04);\n"
"      border-radius: 8px;\n"
"      font-size: 13px;\n"
"    }\n"
"    .history-time {\n"
"      color: var(--text-dim);\n"
"    }\n"
"    .history-mac {\n"
"      font-family: monospace;\n"
"    }\n"
"    .status-badge-mini {\n"
"      padding: 2px 6px;\n"
"      border-radius: 4px;\n"
"      font-size: 11px;\n"
"      font-weight: 600;\n"
"    }\n"
"    .status-badge-mini.success {\n"
"      background: var(--green-bg);\n"
"      color: var(--green);\n"
"    }\n"
"    .footer {\n"
"      text-align: center;\n"
"      margin-top: 40px;\n"
"      font-size: 12px;\n"
"      color: var(--text-dim);\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class='wrapper'>\n"
"    <div class='header'>\n"
"      <div class='brand'>\n"
"        <div class='brand-logo'>MicroLink WoL</div>\n"
"      </div>\n"
"      <div class='status-badge'>\n"
"        <div id='status-dot' class='status-dot'></div>\n"
"        <span id='status-text'>DISCONNECTED</span>\n"
"      </div>\n"
"    </div>\n"
"\n"
"    <div class='grid'>\n"
"      <div class='card'>\n"
"        <div class='card-title'>Device Info</div>\n"
"        <div class='metrics'>\n"
"          <div class='metric'>\n"
"            <span class='metric-label'>Tailscale IP</span>\n"
"            <span id='vpn-ip' class='metric-value'>-.?.?.?-</span>\n"
"          </div>\n"
"          <div class='metric'>\n"
"            <span class='metric-label'>Local IP</span>\n"
"            <span id='local-ip' class='metric-value'>-.?.?.?-</span>\n"
"          </div>\n"
"          <div class='metric'>\n"
"            <span class='metric-label'>Free RAM</span>\n"
"            <span id='free-heap' class='metric-value'>- KB</span>\n"
"          </div>\n"
"          <div class='metric'>\n"
"            <span class='metric-label'>Uptime</span>\n"
"            <span id='uptime' class='metric-value'>-s</span>\n"
"          </div>\n"
"        </div>\n"
"      </div>\n"
"\n"
"      <div class='card'>\n"
"        <div class='card-title'>Trigger Action</div>\n"
"        <div class='form-group'>\n"
"          <label for='mac-input'>Target MAC Address</label>\n"
"          <input type='text' id='mac-input' class='input-field' placeholder='00:11:22:33:44:55'>\n"
"        </div>\n"
"        <button id='btn-wake' class='btn-wake' onclick='triggerWake()'>\n"
"          <span>⚡ Wake Desktop</span>\n"
"        </button>\n"
"      </div>\n"
"\n"
"      <div class='card full-span'>\n"
"        <div class='card-title'>WOL History Log</div>\n"
"        <div id='history-log' class='history-log'>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"\n"
"    <div class='footer'>\n"
"      Powered by ESP32 & Tailscale\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <script>\n"
"    let history = [];\n"
"    try {\n"
"      history = JSON.parse(localStorage.getItem('wol_history')) || [];\n"
"    } catch(e) {}\n"
"\n"
"    function saveHistory() {\n"
"      localStorage.setItem('wol_history', JSON.stringify(history));\n"
"    }\n"
"\n"
"    function renderHistory() {\n"
"      const container = document.getElementById('history-log');\n"
"      if (history.length === 0) {\n"
"        container.innerHTML = '<div style=\"text-align:center;color:var(--text-dim);font-size:14px;padding:20px;\">No wakeup history yet.</div>';\n"
"        return;\n"
"      }\n"
"      container.innerHTML = history.map(item => `\n"
"        <div class=\"history-item\">\n"
"          <span class=\"history-time\">${new Date(item.time).toLocaleTimeString()}</span>\n"
"          <span class=\"history-mac\">${item.mac}</span>\n"
"          <span class=\"status-badge-mini success\">SENT</span>\n"
"        </div>\n"
"      `).join('');\n"
"    }\n"
"\n"
"    async function updateStatus() {\n"
"      try {\n"
"        const res = await fetch('/api/status');\n"
"        const data = await res.json();\n"
"        \n"
"        document.getElementById('vpn-ip').innerText = data.vpn_ip || 'Not Connected';\n"
"        document.getElementById('local-ip').innerText = data.local_ip;\n"
"        document.getElementById('free-heap').innerText = Math.round(data.free_heap / 1024) + ' KB';\n"
"        document.getElementById('uptime').innerText = data.uptime + 's';\n"
"        \n"
"        const dot = document.getElementById('status-dot');\n"
"        const txt = document.getElementById('status-text');\n"
"        if (data.state === 'CONNECTED') {\n"
"          dot.className = 'status-dot connected';\n"
"          txt.innerText = 'ONLINE';\n"
"          txt.style.color = 'var(--green)';\n"
"        } else {\n"
"          dot.className = 'status-dot';\n"
"          txt.innerText = data.state;\n"
"          txt.style.color = '#ef4444';\n"
"        }\n"
"        \n"
"        const macInput = document.getElementById('mac-input');\n"
"        if (!macInput.value && data.default_mac) {\n"
"          macInput.value = data.default_mac;\n"
"        }\n"
"      } catch (e) {\n"
"        console.error('Status fetch failed', e);\n"
"      }\n"
"    }\n"
"\n"
"    async function triggerWake() {\n"
"      const btn = document.getElementById('btn-wake');\n"
"      const macInput = document.getElementById('mac-input');\n"
"      const mac = macInput.value.trim();\n"
"      \n"
"      if (!mac) {\n"
"        alert('Please enter a MAC address');\n"
"        return;\n"
"      }\n"
"      \n"
"      btn.disabled = true;\n"
"      btn.innerText = 'Sending...';\n"
"      \n"
"      try {\n"
"        const res = await fetch(`/api/wake?mac=${encodeURIComponent(mac)}`);\n"
"        const result = await res.json();\n"
"        \n"
"        if (result.status === 'success') {\n"
"          history.unshift({ time: Date.now(), mac: mac });\n"
"          if (history.length > 10) history.pop();\n"
"          saveHistory();\n"
"          renderHistory();\n"
"        } else {\n"
"          alert('Error: ' + result.message);\n"
"        }\n"
"      } catch (e) {\n"
"        alert('Failed to contact ESP32 API.');\n"
"      } finally {\n"
"        btn.disabled = false;\n"
"        btn.innerHTML = '<span>⚡ Wake Desktop</span>';\n"
"      }\n"
"    }\n"
"\n"
"    renderHistory();\n"
"    updateStatus();\n"
"    setInterval(updateStatus, 5000);\n"
"  </script>\n"
"</body>\n"
"</html>\n";

/* ============================================================================
 * Helper function: Parse MAC Address
 * Supports AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF, and AABBCCDDEEFF
 * ========================================================================== */
static bool parse_mac_address(const char *mac_str, uint8_t *mac_out) {
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", 
               &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) == 6 ||
        sscanf(mac_str, "%x-%x-%x-%x-%x-%x", 
               &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac_out[i] = (uint8_t)values[i];
        }
        return true;
    }

    if (strlen(mac_str) == 12) {
        for (int i = 0; i < 6; i++) {
            char tmp[3] = { mac_str[i * 2], mac_str[i * 2 + 1], '\0' };
            char *endptr;
            values[i] = strtol(tmp, &endptr, 16);
            if (*endptr != '\0') return false;
            mac_out[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

/* ============================================================================
 * Send Wake-on-LAN Broadcast Magic Packet
 * ========================================================================== */
static esp_err_t send_wol_packet(const uint8_t *mac_addr) {
    uint8_t packet[102];
    
    // 6 bytes of 0xFF followed by 16 repetitions of target MAC
    memset(packet, 0xFF, 6);
    for (int i = 1; i <= 16; i++) {
        memcpy(&packet[i * 6], mac_addr, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    // Force broadcasting on Wi-Fi STA interface by binding to the Wi-Fi station IP
    if (wifi_ip_addr != 0) {
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(0); // Ephemeral
        local_addr.sin_addr.s_addr = wifi_ip_addr;
        if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
            ESP_LOGW(TAG, "Socket bind to WiFi IP failed: errno %d", errno);
        }
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9); // Port 9 (Standard Wake-on-LAN port)
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255

    int sent = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    close(sock);

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send packet: errno %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WOL Broadcast sent for MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return ESP_OK;
}

/* ============================================================================
 * HTTP Request Handlers
 * ========================================================================== */

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    char json_str[256];
    microlink_state_t state = ml ? microlink_get_state(ml) : ML_STATE_IDLE;
    const char *state_names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR"
    };
    const char *state_name = (state < sizeof(state_names)/sizeof(state_names[0]))
                             ? state_names[state] : "UNKNOWN";

    snprintf(json_str, sizeof(json_str),
             "{\"vpn_ip\":\"%s\",\"local_ip\":\"%s\",\"free_heap\":%lu,\"uptime\":%lu,\"default_mac\":\"%s\",\"state\":\"%s\"}",
             vpn_ip_str, wifi_ip_str,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
             CONFIG_WOL_TARGET_MAC,
             state_name);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    return ESP_OK;
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10;
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10;
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t wake_get_handler(httpd_req_t *req) {
    char query[128];
    char mac_str[32] = "";
    char decoded_mac[32] = "";
    bool success = false;
    char err_msg[64] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "mac", mac_str, sizeof(mac_str)) == ESP_OK) {
            url_decode(decoded_mac, mac_str);
            uint8_t mac[6];
            if (parse_mac_address(decoded_mac, mac)) {
                esp_err_t err = send_wol_packet(mac);
                if (err == ESP_OK) {
                    success = true;
                } else {
                    snprintf(err_msg, sizeof(err_msg), "Failed to broadcast WOL");
                }
            } else {
                snprintf(err_msg, sizeof(err_msg), "Invalid MAC format");
            }
        } else {
            snprintf(err_msg, sizeof(err_msg), "Missing 'mac' query parameter");
        }
    } else {
        snprintf(err_msg, sizeof(err_msg), "Missing query parameters");
    }

    char json_str[256];
    if (success) {
        snprintf(json_str, sizeof(json_str),
                 "{\"status\":\"success\",\"message\":\"WOL packet broadcasted to %s\"}",
                 mac_str);
    } else {
        snprintf(json_str, sizeof(json_str),
                 "{\"status\":\"error\",\"message\":\"%s\"}",
                 err_msg);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    return ESP_OK;
}

#if CONFIG_WOL_LED_STRIP_ENABLED
/* ============================================================================
 * WLED JSON API Handlers — HyperHDR Compatibility
 * ========================================================================== */

/* Helper: Build WLED state JSON fragment into a buffer */
static int wled_build_state_json(char *buf, size_t buflen) {
    return snprintf(buf, buflen,
        "{\"on\":%s,\"bri\":%d,\"transition\":7,\"ps\":-1,\"pl\":-1,"
        "\"seg\":[{\"id\":0,\"start\":0,\"stop\":%d,\"len\":%d,"
        "\"col\":[[255,255,255]],\"fx\":0,\"sx\":128,\"ix\":128,"
        "\"sel\":true,\"on\":true}]}",
        wled_on ? "true" : "false",
        (int)wled_brightness,
        CONFIG_WOL_LED_STRIP_NUM,
        CONFIG_WOL_LED_STRIP_NUM);
}

/* Helper: Build WLED info JSON fragment into a buffer */
static int wled_build_info_json(char *buf, size_t buflen) {
    return snprintf(buf, buflen,
        "{\"ver\":\"0.14.0\",\"vid\":2312080,"
        "\"leds\":{\"count\":%d,\"fps\":30,\"rgbw\":false,\"wv\":0,\"pwr\":0,\"maxpwr\":850},"
        "\"name\":\"MicroLink-WLED\",\"udpport\":21324,\"live\":false,"
        "\"fxcount\":1,\"palcount\":1,"
        "\"arch\":\"esp32\",\"core\":\"v4.4\","
        "\"freeheap\":%lu,\"uptime\":%lu,"
        "\"brand\":\"WLED\",\"product\":\"MicroLink\","
        "\"mac\":\"%s\",\"ip\":\"%s\"}",
        CONFIG_WOL_LED_STRIP_NUM,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
        esp_mac_str,
        wifi_ip_str);
}

/* GET /json — Full WLED response (state + info + effects + palettes) */
static esp_err_t wled_json_get_handler(httpd_req_t *req) {
    char *json_buf = malloc(1024);
    if (!json_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char state_buf[384];
    char info_buf[512];
    wled_build_state_json(state_buf, sizeof(state_buf));
    wled_build_info_json(info_buf, sizeof(info_buf));

    int len = snprintf(json_buf, 1024,
        "{\"state\":%s,\"info\":%s,\"effects\":[\"Solid\"],\"palettes\":[\"Default\"]}",
        state_buf, info_buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buf, len);
    free(json_buf);
    return ESP_OK;
}

/* GET /json/state */
static esp_err_t wled_state_get_handler(httpd_req_t *req) {
    char buf[384];
    int len = wled_build_state_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* POST /json/state — Accept state changes (on/off, brightness) */
static esp_err_t wled_state_post_handler(httpd_req_t *req) {
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    ESP_LOGI(TAG, "WLED state POST: %s", body);

    /* Minimal JSON parsing for "on" and "bri" fields */
    char *on_ptr = strstr(body, "\"on\"");
    if (on_ptr) {
        if (strstr(on_ptr, "true"))  wled_on = true;
        if (strstr(on_ptr, "false")) wled_on = false;
    }

    char *bri_ptr = strstr(body, "\"bri\"");
    if (bri_ptr) {
        bri_ptr = strchr(bri_ptr, ':');
        if (bri_ptr) {
            int bri_val = atoi(bri_ptr + 1);
            if (bri_val >= 0 && bri_val <= 255) {
                wled_brightness = (uint8_t)bri_val;
            }
        }
    }

    /* If turned off, clear the strip immediately */
    if (!wled_on && led_strip_h) {
        led_strip_clear(led_strip_h);
        led_strip_refresh(led_strip_h);
    }

    /* Return updated state */
    char buf[384];
    int len = wled_build_state_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* GET /json/info */
static esp_err_t wled_info_get_handler(httpd_req_t *req) {
    char buf[512];
    int len = wled_build_info_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* GET /json/eff */
static esp_err_t wled_eff_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "[\"Solid\"]");
    return ESP_OK;
}

/* GET /json/pal */
static esp_err_t wled_pal_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "[\"Default\"]");
    return ESP_OK;
}
#endif /* CONFIG_WOL_LED_STRIP_ENABLED */

/* ============================================================================
 * HTTP Server Setup
 * ========================================================================== */
static httpd_handle_t start_webserver(void) {
    if (server != NULL) {
        return server;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16; /* Extra slots for WLED API endpoints */

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t wake_uri = {
        .uri       = "/api/wake",
        .method    = HTTP_GET,
        .handler   = wake_get_handler,
        .user_ctx  = NULL
    };

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &wake_uri);

#if CONFIG_WOL_LED_STRIP_ENABLED
        /* Register WLED-compatible JSON API endpoints */
        httpd_uri_t wled_json_uri = { .uri = "/json", .method = HTTP_GET, .handler = wled_json_get_handler };
        httpd_uri_t wled_state_get_uri = { .uri = "/json/state", .method = HTTP_GET, .handler = wled_state_get_handler };
        httpd_uri_t wled_state_post_uri = { .uri = "/json/state", .method = HTTP_POST, .handler = wled_state_post_handler };
        httpd_uri_t wled_info_uri = { .uri = "/json/info", .method = HTTP_GET, .handler = wled_info_get_handler };
        httpd_uri_t wled_eff_uri = { .uri = "/json/eff", .method = HTTP_GET, .handler = wled_eff_get_handler };
        httpd_uri_t wled_pal_uri = { .uri = "/json/pal", .method = HTTP_GET, .handler = wled_pal_get_handler };

        httpd_register_uri_handler(server, &wled_json_uri);
        httpd_register_uri_handler(server, &wled_state_get_uri);
        httpd_register_uri_handler(server, &wled_state_post_uri);
        httpd_register_uri_handler(server, &wled_info_uri);
        httpd_register_uri_handler(server, &wled_eff_uri);
        httpd_register_uri_handler(server, &wled_pal_uri);
        ESP_LOGI(TAG, "WLED JSON API endpoints registered");
#endif
        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server!");
    return NULL;
}

/* ============================================================================
 * UDP WOL Trigger via Tailscale (Port 9000)
 * ========================================================================== */
static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                       const uint8_t *data, size_t len, void *user_data) {
    char msg[64];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';
    
    // Strip trailing white space/newlines
    while (copy_len > 0 && (msg[copy_len - 1] == '\r' || msg[copy_len - 1] == '\n' || msg[copy_len - 1] == ' ')) {
        msg[copy_len - 1] = '\0';
        copy_len--;
    }

    char src_ip_str[16];
    microlink_ip_to_str(src_ip, src_ip_str);
    ESP_LOGI(TAG, "UDP WOL trigger received from %s:%u: '%s'", src_ip_str, src_port, msg);

    uint8_t mac[6];
    bool valid = false;
    if (parse_mac_address(msg, mac)) {
        valid = true;
    } else if (strcmp(msg, "wake") == 0 || strlen(msg) == 0) {
        // Use default MAC address
        if (parse_mac_address(CONFIG_WOL_TARGET_MAC, mac)) {
            valid = true;
            strncpy(msg, CONFIG_WOL_TARGET_MAC, sizeof(msg) - 1);
        }
    }

    char reply[128];
    if (valid) {
        esp_err_t err = send_wol_packet(mac);
        if (err == ESP_OK) {
            snprintf(reply, sizeof(reply), "SUCCESS: WOL packet sent to %s\n", msg);
        } else {
            snprintf(reply, sizeof(reply), "ERROR: Failed to broadcast WOL packet\n");
        }
    } else {
        snprintf(reply, sizeof(reply), "ERROR: Invalid MAC format '%s'. Expected AA:BB:CC:DD:EE:FF\n", msg);
    }

    microlink_udp_send(sock, src_ip, src_port, reply, strlen(reply));
}

/* ============================================================================
 * Callbacks
 * ========================================================================== */

static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    const char *state_names[] = {
        "IDLE", "WIFI_WAIT", "CONNECTING", "REGISTERING",
        "CONNECTED", "RECONNECTING", "ERROR"
    };
    const char *name = (state < sizeof(state_names)/sizeof(state_names[0]))
                       ? state_names[state] : "UNKNOWN";
    ESP_LOGI(TAG, "MicroLink state changed to: %s", name);

    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        microlink_ip_to_str(ip, vpn_ip_str);
        ESP_LOGI(TAG, "Tailscale VPN connected. IP: %s", vpn_ip_str);
    } else {
        strcpy(vpn_ip_str, "0.0.0.0");
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                             void *user_data) {
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "Peer update: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}

/* ============================================================================
 * WiFi Setup
 * ========================================================================== */

static void wifi_try_next(void) {
    if (wifi_list_count <= 1) {
        esp_wifi_connect();
        return;
    }

    wifi_retry_count++;
    if (wifi_retry_count >= WIFI_MAX_RETRIES_PER_SSID) {
        wifi_retry_count = 0;
        current_wifi_idx = (current_wifi_idx + 1) % wifi_list_count;
    }

    ml_config_wifi_entry_t *e = &wifi_list.entries[current_wifi_idx];
    wifi_config_t wifi_config = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    strncpy((char *)wifi_config.sta.ssid, e->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, e->pass, sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "WiFi trying #%d/%d: %s (retry %d/%d)",
             current_wifi_idx + 1, wifi_list_count, e->ssid,
             wifi_retry_count + 1, WIFI_MAX_RETRIES_PER_SSID);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
        wifi_try_next();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_ip_addr = event->ip_info.ip.addr;
        snprintf(wifi_ip_str, sizeof(wifi_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        
        ESP_LOGI(TAG, "WiFi connected to SSID: %s, Local IP: %s",
                 wifi_list_count > 0 ? wifi_list.entries[current_wifi_idx].ssid : wifi_ssid,
                 wifi_ip_str);
        
        wifi_retry_count = 0;
        
        // Start Web UI immediately on local network
        start_webserver();
        
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

#if CONFIG_WOL_LED_STRIP_ENABLED
static led_strip_handle_t led_strip_h = NULL;

static void led_strip_init(void) {
    /* LED strip common configuration */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_WOL_LED_STRIP_GPIO,
        .max_leds = CONFIG_WOL_LED_STRIP_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    /* RMT backend configuration */
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz resolution
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_h));
    led_strip_clear(led_strip_h);
    ESP_LOGI(TAG, "WS2812B LED strip initialized on GPIO %d, %d LEDs", 
             CONFIG_WOL_LED_STRIP_GPIO, CONFIG_WOL_LED_STRIP_NUM);

    /* Read ESP32 MAC address for WLED info response */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(esp_mac_str, sizeof(esp_mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "WLED emulation MAC: %s", esp_mac_str);
}

/* Helper: Apply brightness scaling and write pixel to strip */
static inline void led_set_pixel_with_brightness(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (wled_brightness < 255) {
        r = (uint8_t)((uint16_t)r * wled_brightness / 255);
        g = (uint8_t)((uint16_t)g * wled_brightness / 255);
        b = (uint8_t)((uint16_t)b * wled_brightness / 255);
    }
    led_strip_set_pixel(led_strip_h, idx, r, g, b);
}

/* ============================================================================
 * UDPRAW Receiver Task (Port 19446) — Legacy HyperHDR support
 * ========================================================================== */
static void led_udp_task(void *pvParameters) {
    uint8_t rx_buffer[1500];
    struct sockaddr_in dest_addr;

    while (1) {
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(CONFIG_WOL_LED_UDP_PORT);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket for LED UDP: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Set socket receive timeout
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket bind failed for LED UDP: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "HyperHDR UDPRAW receiver listening on port %d", CONFIG_WOL_LED_UDP_PORT);

        while (1) {
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                // Timeout or error, just loop
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                    break;
                }
                continue;
            }

            if (!wled_on) continue; /* Respect WLED on/off state */

            // Parse raw RGB packet
            // Packet should contain 3 bytes (R, G, B) per LED.
            int num_leds_received = len / 3;
            int limit = (num_leds_received < CONFIG_WOL_LED_STRIP_NUM) ? num_leds_received : CONFIG_WOL_LED_STRIP_NUM;

            for (int i = 0; i < limit; i++) {
                led_set_pixel_with_brightness(i, rx_buffer[i * 3], rx_buffer[i * 3 + 1], rx_buffer[i * 3 + 2]);
            }

            // Clear any remaining LEDs if packet is shorter than configured strip
            if (limit < CONFIG_WOL_LED_STRIP_NUM) {
                for (int i = limit; i < CONFIG_WOL_LED_STRIP_NUM; i++) {
                    led_strip_set_pixel(led_strip_h, i, 0, 0, 0);
                }
            }

            led_strip_refresh(led_strip_h);
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

/* ============================================================================
 * DDP (Distributed Display Protocol) Receiver Task — WLED Compatible
 * Port 4048, 10-byte header + raw RGB payload
 * ========================================================================== */
#define DDP_HEADER_LEN     10
#define DDP_FLAGS1_VER1    0x40
#define DDP_FLAGS1_PUSH    0x01

static void led_ddp_task(void *pvParameters) {
    uint8_t rx_buffer[1500];
    struct sockaddr_in dest_addr;

    while (1) {
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(CONFIG_WOL_LED_DDP_PORT);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create DDP socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "DDP socket bind failed: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "DDP receiver listening on port %d (WLED-compatible)", CONFIG_WOL_LED_DDP_PORT);

        while (1) {
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE(TAG, "DDP recvfrom failed: errno %d", errno);
                    break;
                }
                continue;
            }

            /* Need at least the DDP header */
            if (len < DDP_HEADER_LEN) continue;
            if (!wled_on) continue; /* Respect WLED on/off state */

            /* Parse DDP header */
            uint8_t flags1 = rx_buffer[0];
            /* uint8_t flags2 = rx_buffer[1]; — reserved */
            /* uint8_t type   = rx_buffer[2]; */
            /* uint8_t id     = rx_buffer[3]; */
            uint32_t offset = ((uint32_t)rx_buffer[4] << 24) |
                              ((uint32_t)rx_buffer[5] << 16) |
                              ((uint32_t)rx_buffer[6] << 8)  |
                              ((uint32_t)rx_buffer[7]);
            uint16_t data_len = ((uint16_t)rx_buffer[8] << 8) | rx_buffer[9];

            /* Pixel data starts after header */
            uint8_t *pixel_data = rx_buffer + DDP_HEADER_LEN;
            int available = len - DDP_HEADER_LEN;
            if (data_len > available) data_len = available;

            /* Offset is in bytes; each pixel = 3 bytes (RGB) */
            int pixel_offset = offset / 3;
            int num_pixels = data_len / 3;
            int limit = pixel_offset + num_pixels;
            if (limit > CONFIG_WOL_LED_STRIP_NUM) limit = CONFIG_WOL_LED_STRIP_NUM;

            for (int i = pixel_offset; i < limit; i++) {
                int data_idx = (i - pixel_offset) * 3;
                led_set_pixel_with_brightness(i, pixel_data[data_idx], pixel_data[data_idx + 1], pixel_data[data_idx + 2]);
            }

            /* Push flag: frame complete, refresh the strip */
            if (flags1 & DDP_FLAGS1_PUSH) {
                led_strip_refresh(led_strip_h);
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "DDP socket error, restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}
#endif /* CONFIG_WOL_LED_STRIP_ENABLED */

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

#if CONFIG_WOL_STATIC_IP_ENABLED
    esp_netif_dhcpc_stop(sta_netif);

    esp_ip4_addr_t ip, netmask, gw;
    esp_netif_str_to_ip4(CONFIG_WOL_STATIC_IP, &ip);
    esp_netif_str_to_ip4(CONFIG_WOL_STATIC_NETMASK, &netmask);
    esp_netif_str_to_ip4(CONFIG_WOL_STATIC_GATEWAY, &gw);

    esp_netif_ip_info_t ip_info;
    ip_info.ip = ip;
    ip_info.netmask = netmask;
    ip_info.gw = gw;
    esp_netif_set_ip_info(sta_netif, &ip_info);

    esp_netif_dns_info_t dns_info;
    esp_netif_str_to_ip4(CONFIG_WOL_STATIC_GATEWAY, &dns_info.ip.u_addr.ip4);
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    ESP_LOGI(TAG, "Static IP configured: %s, GW: %s, Mask: %s",
             CONFIG_WOL_STATIC_IP, CONFIG_WOL_STATIC_GATEWAY, CONFIG_WOL_STATIC_NETMASK);
#else
    (void)sta_netif;
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable WiFi power save mode for maximum responsiveness on Tailscale / WireGuard
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi initialized (PS=NONE), connecting to SSID '%s'...", wifi_ssid);
}

/* ============================================================================
 * Main entry point
 * ========================================================================== */

void app_main(void) {
    /* Initialize NVS for Wi-Fi configurations and MicroLink secrets */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting MicroLink Tailscale WOL Proxy");
    ESP_LOGI(TAG, "Free SRAM heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* Check NVS for stored Wi-Fi configurations */
    memset(&wifi_list, 0, sizeof(wifi_list));
    wifi_list.active_idx = 0xFF;

    if (ml_config_get_wifi_list(&wifi_list) && wifi_list.count > 1) {
        wifi_list_count = wifi_list.count;
        current_wifi_idx = 0;
        strncpy(wifi_ssid, wifi_list.entries[0].ssid, sizeof(wifi_ssid) - 1);
        strncpy(wifi_password, wifi_list.entries[0].pass, sizeof(wifi_password) - 1);
        ESP_LOGI(TAG, "WiFi multi-SSID configured in NVS: %d networks", wifi_list_count);
    } else if (ml_config_get_nvs_wifi(wifi_ssid, sizeof(wifi_ssid),
                                       wifi_password, sizeof(wifi_password))) {
        ESP_LOGI(TAG, "Using NVS Wi-Fi credentials: %s", wifi_ssid);
    } else {
        ESP_LOGI(TAG, "Using Kconfig default Wi-Fi credentials: %s", wifi_ssid);
    }

    /* Initialize local Wi-Fi interface */
    wifi_init();

    /* Wait until local network is established */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

#if CONFIG_WOL_LED_STRIP_ENABLED
    /* Initialize the LED strip */
    led_strip_init();

    /* Start the UDPRAW receiver task (legacy, port 19446) */
    xTaskCreate(led_udp_task, "led_udp", 4096, NULL, 5, NULL);

    /* Start the DDP receiver task (WLED-compatible, port 4048) */
    xTaskCreate(led_ddp_task, "led_ddp", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "WLED API emulation active — DDP port %d, UDPRAW port %d",
             CONFIG_WOL_LED_DDP_PORT, CONFIG_WOL_LED_UDP_PORT);
#endif

    /* Initialize MicroLink Tailscale Client */
    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 13, // Lower power for temperature control
    };

    ml = microlink_init(&config);
    if (!ml) {
        ESP_LOGE(TAG, "Failed to initialize MicroLink!");
        return;
    }

    microlink_set_state_callback(ml, on_state_change, NULL);
    microlink_set_peer_callback(ml, on_peer_update, NULL);

    ESP_ERROR_CHECK(microlink_start(ml));

    /* Wait for connected status to register the UDP socket */
    while (!microlink_is_connected(ml)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Register the UDP socket listener on port CONFIG_WOL_UDP_PORT */
    udp_sock = microlink_udp_create(ml, CONFIG_WOL_UDP_PORT);
    if (!udp_sock) {
        ESP_LOGE(TAG, "Failed to bind Tailscale UDP socket on port %d!", CONFIG_WOL_UDP_PORT);
    } else {
        ESP_LOGI(TAG, "Tailscale UDP listener bound to port %d", CONFIG_WOL_UDP_PORT);
        microlink_udp_set_rx_callback(udp_sock, on_udp_rx, NULL);
    }

    /* Run diagnostics reporting every 30s */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (microlink_is_connected(ml)) {
            ESP_LOGI(TAG, "VPN Connected | Heap Free: %lu bytes | Peers Connected: %d",
                     (unsigned long)esp_get_free_heap_size(),
                     microlink_get_peer_count(ml));
        } else {
            ESP_LOGI(TAG, "VPN Status: %d | Heap Free: %lu bytes",
                     (int)microlink_get_state(ml),
                     (unsigned long)esp_get_free_heap_size());
        }
    }
}
