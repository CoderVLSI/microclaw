![TimiClaw Banner](repo-banner.png)

# TimiClaw (Metal Core)

The first high-performance, autonomous agent framework for microcontrollers. Hard real-time, zero OS overhead, ultra-secure.

## What this does now

- Runs a local agent loop on ESP32.
- Accepts Telegram text commands.
- Executes strict allowlisted tools only:
  - `status`
  - `relay_set <pin> <0|1>`
  - `sensor_read <pin>`
  - `plan <task>` (optional LLM-powered coding plan)
  - `webjob_set_daily <HH:MM> <task>` (schedule backend-driven web task)
  - `webjob_show`, `webjob_run`, `webjob_clear`
  - `remember <note>` (persist note in on-device memory)
  - `memory` (show saved notes)
  - `forget` (clear saved notes)
  - `search <query>` (Tavily/DDG web search)
  - `time` (Get current time/date)
- **Web Dashboard**:
  - Accessible at `http://<ESP32-IP>/`.
  - Chat interface with history.
  - Configuration management (API Keys, Models).
  - Real-time system status (Heap, WiFi, Uptime).
- **Robust Architecture**:
  - Agent logic runs in a dedicated FreeRTOS Task (16KB stack) to prevent WDT resets and stack overflows.
  - Asynchronous message queueing for high concurrency.
- **OTA Updates**: Supports firmware updates via `pio run -t upload`.

## Setup

1. Install PlatformIO (VS Code extension or CLI).
2. Open this folder in PlatformIO.
3. Create a local `.env` file from the example:

```powershell
copy .env.example .env
```

4. Edit `.env` with Wi-Fi + Telegram bot token + allowed chat id.
5. Optional: enable LLM provider for `plan` command:
   - `LLM_PROVIDER=openai|anthropic|gemini|glm`
   - `LLM_API_KEY=...`
   - `LLM_MODEL=...` (provider-specific)
   - adjust base URL variables if needed
6. Optional: configure a backend endpoint for scheduled web jobs:
   - `WEB_JOB_ENDPOINT_URL=https://your-server.example`
   - `WEB_JOB_API_KEY=...` (optional)
   - endpoint expected: `POST /run_job` with JSON payload `{"task","timezone","device"}`
   - response should return one of: `reply`, `result`, `text`, or `content`
7. Optional: direct on-device web search provider (no backend):
   - `WEB_SEARCH_PROVIDER=auto|tavily|ddg`
   - `WEB_SEARCH_API_KEY=...` (required for Tavily)
   - `WEB_SEARCH_BASE_URL=https://api.tavily.com`
   - if backend URL is empty, firmware uses provider flow: `tavily -> ddg fallback`
8. Build and flash Firmware AND Filesystem (for Dashboard):

```powershell
pio run -t upload
pio run -t uploadfs
pio device monitor -b 115200
```

## Notes

- TLS currently uses `client.setInsecure()` for quick bring-up. Replace with pinned cert/CA before production.
- Telegram parser is intentionally small and conservative for RAM limits.
- This is a WROOM-safe baseline, not full OpenClaw parity.
- `.env` is ignored via `.gitignore`.
- Build step generates `include/brain_secrets.generated.h` from `.env` via `scripts/load_env.py`.
- Provider endpoint defaults:
  - OpenAI: `https://api.openai.com/v1/chat/completions`
  - Anthropic: `https://api.anthropic.com/v1/messages`
  - Gemini: `https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent`
  - GLM (Z.ai coding plan): `https://api.z.ai/api/coding/paas/v4`
- Memory is persisted in NVS and is injected into `/plan` context.
- **Dashboard**: The web dashboard is a Single Page Application (SPA) served from SPIFFS.

