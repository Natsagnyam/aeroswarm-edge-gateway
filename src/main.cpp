// cat << 'EOF' > src/main.cpp
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <sys/socket.h>

// AM335x GPIO1 Addresses & Pins
#define GPIO1_START_ADDR 0x4804C000
#define GPIO1_END_ADDR   0x4804CFFF
#define GPIO1_SIZE (GPIO1_END_ADDR - GPIO1_START_ADDR)

#define GPIO_DATAIN      0x138
#define GPIO_SETDATAOUT  0x194
#define GPIO_CLEARDATAOUT 0x190

#define LED3_PIN         (1 << 24) // USR3 LED (GPIO1_24)
#define INPUT_PIN        (1 << 28) // Digital Input P9_12 (GPIO1_28)

static volatile uint32_t *gpio1_addr = nullptr;
static long server_start_time = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();

const std::string HTML_PAGE = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Canonical Ubuntu IoT Silicon Gateway Dashboard</title>
    <style>
        :root {
            --ubuntu-orange: #E95420;
            --ubuntu-dark: #111111;
            --ubuntu-card: #1E1E1E;
            --text-main: #F7F7F7;
            --accent-green: #0E8420;
            --accent-red: #A81212;
        }
        body {
            font-family: 'Ubuntu', -apple-system, sans-serif;
            background-color: var(--ubuntu-dark);
            color: var(--text-main);
            margin: 0;
            padding: 20px;
            display: flex;
            justify-content: center;
        }
        .container {
            width: 100%;
            max-width: 850px;
            background: var(--ubuntu-card);
            border-top: 4px solid var(--ubuntu-orange);
            border-radius: 8px;
            padding: 24px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.5);
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-bottom: 1px solid #333;
            padding-bottom: 16px;
            margin-bottom: 24px;
        }
        .header h1 {
            margin: 0;
            font-size: 1.4rem;
            color: var(--ubuntu-orange);
        }
        .status-badge {
            background: rgba(14, 132, 32, 0.2);
            color: #47D45A;
            border: 1px solid var(--accent-green);
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 0.85rem;
            font-weight: bold;
        }
        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 24px;
        }
        .card {
            background: #252525;
            border: 1px solid #333;
            border-radius: 6px;
            padding: 16px;
        }
        .card h3 {
            margin-top: 0;
            font-size: 0.95rem;
            color: #AAA;
            text-transform: uppercase;
        }
        .metric {
            font-size: 1.6rem;
            font-weight: bold;
            color: #FFF;
            margin: 8px 0;
        }
        .sub-metric {
            font-size: 1rem;
            color: #DDD;
            margin: 4px 0;
        }
        .input-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 4px;
            font-weight: bold;
            font-size: 0.9rem;
            margin-left: 6px;
        }
        .input-low { background: #333; color: #AAA; border: 1px solid #555; }
        .input-high { background: rgba(14, 132, 32, 0.3); color: #47D45A; border: 1px solid #0E8420; }
        .btn-toggle {
            width: 100%;
            background: var(--ubuntu-orange);
            color: white;
            border: none;
            padding: 14px;
            font-size: 1rem;
            font-weight: bold;
            border-radius: 4px;
            cursor: pointer;
        }
        .btn-toggle:hover { background: #C74316; }
        .btn-off { background: #444; }
        .console-box {
            background: #000;
            border: 1px solid #333;
            border-radius: 4px;
            padding: 12px;
            font-family: 'Ubuntu Mono', monospace;
            font-size: 0.85rem;
            color: #00FF66;
            height: 120px;
            overflow-y: auto;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Canonical Ubuntu Silicon Integration Portal</h1>
            <span class="status-badge">ONLINE (Tailscale Mesh)</span>
        </div>
        <div class="grid">
            <div class="card">
                <h3>Hardware I/O Control & Status</h3>
                <div class="metric" id="led-status-text">LED 3: OFF</div>
                <button class="btn-toggle" id="led-btn" onclick="toggleLED()">Turn On USR3 LED</button>
                <div style="margin-top: 15px;" class="sub-metric">
                    Digital Input (P9_12): <span id="input-status-badge" class="input-badge input-low">LOW (0)</span>
                </div>
            </div>
            <div class="card">
                <h3>Live Auto-Refresh Telemetry</h3>
                <div class="sub-metric">Board: <strong>BeagleBone Black</strong></div>
                <div class="sub-metric">CPU Temp: <strong id="cpu-temp">-- °C</strong></div>
                <div class="sub-metric">Server Uptime: <strong id="uptime-sec">-- s</strong></div>
                <div class="sub-metric">Polling: <strong style="color: #47D45A;">Active (2s)</strong></div>
            </div>
        </div>
        <h3>Real-time REST API Log</h3>
        <div class="console-box" id="console-log">
            [System] Dashboard initialized. Monitoring MMIO Input P9_12...<br>
        </div>
    </div>
    <script>
        let ledState = false;
        let lastInputState = null;
        const token = "Natska_key";

        function logMsg(msg) {
            const cb = document.getElementById("console-log");
            cb.innerHTML += `[${new Date().toLocaleTimeString()}] ${msg}<br>`;
            cb.scrollTop = cb.scrollHeight;
        }

        async function toggleLED() {
            ledState = !ledState;
            try {
                let res = await fetch('/api/led', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json', 'Authorization': `Bearer ${token}`},
                    body: JSON.stringify({ led: 3, state: ledState })
                });
                let data = await res.json();
                if (res.ok) {
                    document.getElementById("led-status-text").innerText = `LED 3: ${ledState ? "ON" : "OFF"}`;
                    document.getElementById("led-btn").className = ledState ? "btn-toggle btn-off" : "btn-toggle";
                    document.getElementById("led-btn").innerText = ledState ? "Turn Off USR3 LED" : "Turn On USR3 LED";
                    logMsg(`LED 3 toggled: ${ledState}`);
                } else {
                    logMsg(`Error: ${data.error}`);
                }
            } catch (e) { logMsg(`Network error: ${e.message}`); }
        }

        async function fetchTelemetry() {
            try {
                let res = await fetch('/api/telemetry');
                let data = await res.json();
                document.getElementById("cpu-temp").innerText = data.cpu_temp + " °C";
                document.getElementById("uptime-sec").innerText = data.uptime + " s";

                // Update Digital Input UI
                const badge = document.getElementById("input-status-badge");
                if (data.digital_in === 1) {
                    badge.innerText = "HIGH (1)";
                    badge.className = "input-badge input-high";
                } else {
                    badge.innerText = "LOW (0)";
                    badge.className = "input-badge input-low";
                }

                if (lastInputState !== null && lastInputState !== data.digital_in) {
                    logMsg(`MMIO State Event: P9_12 changed to ${data.digital_in ? "HIGH" : "LOW"}`);
                }
                lastInputState = data.digital_in;
            } catch (e) {}
        }

        setInterval(fetchTelemetry, 2000);
        fetchTelemetry();
    </script>
</body>
</html>
)html";

bool init_hardware() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return false;
    void *map_base = mmap(0, GPIO1_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO1_START_ADDR);
    close(fd);
    if (map_base == MAP_FAILED) return false;
    gpio1_addr = (volatile uint32_t *)map_base;
    return true;
}

void set_led3(bool state) {
    if (!gpio1_addr) return;
    if (state) gpio1_addr[GPIO_SETDATAOUT / 4] = LED3_PIN;
    else gpio1_addr[GPIO_CLEARDATAOUT / 4] = LED3_PIN;
}

int read_digital_input() {
    if (!gpio1_addr) return 0;
    uint32_t datain = gpio1_addr[GPIO_DATAIN / 4];
    return (datain & INPUT_PIN) ? 1 : 0;
}

int main() {
    if (!init_hardware()) return 1;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        char buffer[2048] = {0};
        read(client_fd, buffer, sizeof(buffer) - 1);
        std::string request(buffer);
        std::string response;

        if (request.find("GET / ") != std::string::npos || request.find("GET /index.html") != std::string::npos) {
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(HTML_PAGE.length()) + "\r\nConnection: close\r\n\r\n" + HTML_PAGE;
        } else if (request.find("GET /api/telemetry") != std::string::npos) {
            long uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - server_start_time;
            int input_val = read_digital_input();
            std::string json = "{\"status\":\"online\",\"cpu_temp\":43.2,\"uptime\":" + std::to_string(uptime) + ",\"digital_in\":" + std::to_string(input_val) + ",\"board\":\"BeagleBone Black\"}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
        } else if (request.find("POST /api/led") != std::string::npos) {
            if (request.find("Authorization: Bearer Natska_key") == std::string::npos) {
                std::string err = "{\"error\":\"Unauthorized.\"}";
                response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(err.length()) + "\r\nConnection: close\r\n\r\n" + err;
            } else {
                bool state = (request.find("\"state\":true") != std::string::npos || request.find("\"state\": true") != std::string::npos);
                set_led3(state);
                std::string success = "{\"status\":\"success\",\"state\":" + std::string(state ? "true" : "false") + "}";
                response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(success.length()) + "\r\nConnection: close\r\n\r\n" + success;
            }
        } else {
            std::string err = "{\"error\":\"Not Found\"}";
            response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(err.length()) + "\r\nConnection: close\r\n\r\n" + err;
        }
        write(client_fd, response.c_str(), response.length());
        close(client_fd);
    }
    return 0;
}
