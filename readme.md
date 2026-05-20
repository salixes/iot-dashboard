# TROYIKZ — Complete Integration Guide
*Smart Smoke Detection & Hazard Classification System*

---

## Files in This Package

| File | Purpose |
|------|---------|
| `index.html` | Dashboard (your existing file) |
| `supabase_setup.sql` | Run once in Supabase SQL Editor |
| `troyikz_smoke_detector.ino` | Upload to ESP32 via Arduino IDE |

---

## Step 1 — Supabase Setup

1. Go to **https://supabase.com** → New Project → name it `troyikz`
2. Open **SQL Editor** → paste entire `supabase_setup.sql` → click **Run**
3. Go to **Database → Replication** → confirm `smoke_logs` and `sensor_status` are listed (the SQL adds them automatically)
4. Go to **Project Settings → API** → copy:
   - **Project URL** → looks like `https://abcdefgh.supabase.co`
   - **anon public key** → long JWT string

---

## Step 2 — Update Dashboard Credentials

Open `index.html` and find these two lines near the top of the `<script>` block:

```javascript
const SUPABASE_URL  = 'https://YOUR_PROJECT_ID.supabase.co';
const SUPABASE_ANON = 'YOUR_SUPABASE_ANON_KEY';
```

Replace with your actual values from Step 1.

---

## Step 3 — Arduino IDE Setup

### Install ESP32 Board Support
1. Arduino IDE → **File → Preferences**
2. Add to *Additional Boards Manager URLs*:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board Manager** → search `esp32` → install **esp32 by Espressif Systems**

### Install Required Library
- **Sketch → Include Library → Manage Libraries**
- Search `ArduinoJson` → install version **6.x** by Benoit Blanchon

### Configure the Sketch
Open `troyikz_smoke_detector.ino` and update the 4 config lines at the top:

```cpp
const char* WIFI_SSID         = "YOUR_WIFI_SSID";       // your 2.4GHz network
const char* WIFI_PASSWORD     = "YOUR_WIFI_PASSWORD";
const char* SUPABASE_URL      = "https://YOUR_PROJECT_ID.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";
```

### Upload Settings
| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 921600 |
| CPU Frequency | 240MHz |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs |

---

## Step 4 — Wiring

```
ESP32 Pin        Wire To
─────────────────────────────────────────
GPIO 34 (ADC1)  → MQ Sensor  AOUT
3.3V            → MQ Sensor  VCC
GND             → MQ Sensor  GND

GPIO 26         → 330Ω resistor → Green LED (+)
GND             → Green LED (-)

GPIO 27         → 330Ω resistor → Red LED (+)
GND             → Red LED (-)

GPIO 25         → Active Buzzer (+)
GND             → Active Buzzer (-)
```

> **Important notes:**
> - Use **ADC1 pins only** (GPIO 32–39). ADC2 conflicts with WiFi and will return garbage values.
> - Use an **active** buzzer (has internal oscillator). Passive buzzers need PWM — active ones work directly with `digitalWrite`.
> - The 330Ω resistors protect the GPIO pins and LEDs.

---

## How the System Works Together

```
[MQ Sensor]
    ↓  analog voltage
[ESP32]
    ↓  reads ADC, classifies, drives LEDs/buzzer
    ↓  HTTP POST on Stop
[Supabase smoke_logs table]
    ↓  Realtime INSERT event
[Dashboard (Supabase JS client)]
    ↓  handleNewLog() triggered instantly
[Screen updates — chart, LEDs, banner, table]
```

### Session Lifecycle

| Step | Dashboard | ESP32 | Supabase |
|------|-----------|-------|----------|
| Warm-up (3 min) | Shows countdown overlay | Heats sensor element | Receives heartbeat every 5s |
| Press **Start Testing** | Session ID created, banner → "Testing" | Starts reading loop | Nothing yet |
| During session | Live chart updates (demo or real) | Reads sensor every 800ms, drives LEDs | Nothing (no writes during session) |
| Press **Stop Testing** | Session marked Completed | Final reading taken, HTTP POST fired | Record inserted, Realtime triggers dashboard |
| Dashboard receives event | Table row added, donut chart updates, counters update | LEDs turn off after 2s | Row visible in `smoke_logs` |

---

## Smoke Classification Logic

| Condition | ADC Value | Classification | LED | Buzzer |
|-----------|-----------|----------------|-----|--------|
| Match smoke / clean air | < 400 | Good Smoke | 🟢 Green ON | OFF |
| Gas / chemical smoke | ≥ 400 | Bad Smoke | 🔴 Red ON | ON |

Change the threshold in both files if needed:
- **Dashboard:** `const SMOKE_THRESHOLD = 400;`
- **ESP32:** `const int SMOKE_THRESHOLD = 400;`

---

## Serial Monitor Commands

With Serial Monitor open at **115200 baud**:

| Key | Action |
|-----|--------|
| `S` | Start testing session |
| `X` | Stop testing session (saves record) |

Useful for testing without the dashboard open.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Dashboard shows "Supabase not configured" | Check URL and anon key in `index.html` |
| ESP32 won't connect to WiFi | Must be 2.4GHz (not 5GHz); check SSID/password |
| Sensor always reads 0 | Wrong pin — use GPIO 32–39 (ADC1 only) |
| Supabase POST returns 401 | Check anon key; verify RLS policies were created |
| Realtime not updating | Go to Supabase → Database → Replication → verify tables are listed |
| PDF download fails | Use Chrome or Firefox; Safari blocks blob downloads |
| `ArduinoJson` compile error | Must be version 6.x, not 5.x or 7.x |

---

## Future Sensor Expansion

The code already has commented slots ready for:

| Sensor | Detects | Add to GPIO |
|--------|---------|-------------|
| MQ-7 | Carbon monoxide | GPIO 35 |
| MQ-135 | Air quality (NH3, NOx, benzene) | GPIO 32 |
| DHT22 | Temperature + Humidity | GPIO 33 (digital) |

Just uncomment and add the extra fields to the Supabase POST payload.