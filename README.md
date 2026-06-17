# ESP32 CYD Stock Ticker

A stock and cryptocurrency price tracker for the ESP32 Cheap Yellow Display (CYD / ESP32-2432S028).
Displays live prices, percentage change, and portfolio value on the built-in 2.8" TFT touchscreen.
Configured entirely via a browser, no code changes required.

![ESP32 CYD Stock Ticker showing 3 tickers](https://cdn.shopify.com/s/files/1/0870/0021/9940/files/20260617_125927.jpg?v=1781667798)

## Demo

[![ESP32 CYD Stock Ticker Demo](https://img.youtube.com/vi/qng6zG75FMI/maxresdefault.jpg)](https://www.youtube.com/watch?v=qng6zG75FMI)

## Features

- Live stock prices via Finnhub (free API key required)
- Live crypto prices via CoinGecko (no key required)
- Touch to drill into detail view for any ticker
- Portfolio mode — track holdings value and day P&L
- Price alerts with LED flash on breach
- Dark and light mode
- Full web UI for configuration — change tickers, refresh rate, brightness, holdings and alerts from any browser on your network
- JSON API endpoint at `/api/quotes` for home automation integration
- WiFiManager captive portal — no hardcoded credentials

![ESP32 CYD Stock Ticker showing 8 tickers in 2 column grid](https://cdn.shopify.com/s/files/1/0870/0021/9940/files/20260617_130053.jpg?v=1781667798)

![ESP32 CYD Stock Ticker portfolio mode](https://cdn.shopify.com/s/files/1/0870/0021/9940/files/20260617_130153.jpg?v=1781667798)

![ESP32 CYD Stock Ticker light mode](https://cdn.shopify.com/s/files/1/0870/0021/9940/files/20260617_130115.jpg?v=1781667798)

## Hardware

- [ESP32 CYD (ESP32-2432S028)](https://zaitronics.com.au/collections/esp32/products/esp32-with-2-8-lcd-tft-touch-screen-capacitive-wifi-bluetooth-dev-board) — everything is built in, no wiring required
- USB-C cable and power supply

## Quick Start

1. Flash the firmware (see build guide below)
2. Connect to the `StockTicker-Setup` WiFi access point and enter your WiFi credentials and Finnhub API key
3. Get a free Finnhub API key at [finnhub.io](https://finnhub.io)
4. Open the displayed IP address in your browser to configure tickers

**Full build guide:** https://zaitronics.com.au/blogs/guides/esp32-cyd-stock-ticker

## Supported Ticker Formats

| Type | Format | Example |
|------|--------|---------|
| US Stocks | Symbol | `AAPL` |
| Crypto | `CRYPTO:coingecko-id` | `CRYPTO:bitcoin` |
| Forex | Finnhub format | `OANDA:EUR_USD` |

## API Rate Limits

- Finnhub free: 60 req/min
- CoinGecko free: 30 req/min
- Default refresh: 15s (safe for up to 8 tickers on both free tiers)
- Minimum refresh: 10s

## Dependencies

Install via Arduino Library Manager:

- TFT_eSPI
- XPT2046_Touchscreen
- ArduinoJson
- WiFiManager (tzapu)

Board: `ESP32 Dev Module` via ESP32 Arduino core

## TFT_eSPI Configuration

Copy `User_Setup.h` from the `/config` folder into your TFT_eSPI library folder before compiling.
This configures the correct pins for the CYD.

## License

MIT — free to use, modify, and distribute.

---

Built by [Zaitronics](https://zaitronics.com.au) — electronics components and maker supplies, Melbourne AU.
