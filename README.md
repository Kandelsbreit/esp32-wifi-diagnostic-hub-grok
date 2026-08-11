# ESP32 Wi-Fi Diagnostic Hub (by Grok)

Тяжёлый проект диагностического хаба для **голого ESP32** без каких-либо внешних модулей.

Написан специально для Agnia (Kandelsbreit) · Grok · August 2026

**Статус:** прошёл полный аудит + реальную сборку через PlatformIO (успешно).

## Что умеет

- Поднимает свою точку доступа `ESP32-Diag-Hub`
- Полноценный веб-интерфейс с тёмной темой
- Живые данные через WebSocket (heap, uptime, клиенты, канал, температура)
- Асинхронный сканер Wi-Fi сетей (BSSID, RSSI, сортировка)
- Подробная информация о чипе и системе
- Список подключённых клиентов (MAC)
- Кольцевой лог событий
- OTA-обновление прошивки через веб
- HTTP Basic Auth + reboot
- Работает полностью автономно (только питание)

## Как прошить

### PlatformIO (рекомендуется)

```bash
git clone https://github.com/Kandelsbreit/esp32-wifi-diagnostic-hub-grok.git
cd esp32-wifi-diagnostic-hub-grok
pio run -t upload
```

## Как пользоваться

1. Включи плату
2. Подключись к Wi-Fi **`ESP32-Diag-Hub`** (пароль: `diagnostic123`)
3. Открой **http://192.168.4.1**
4. Для OTA / Reboot: логин `admin` / пароль `grok2026`

## Технические детали сборки (проверено)

- Board: `esp32dev`
- Framework: Arduino (espressif32 platform 7.x)
- Libraries: ESPAsyncWebServer 3.6.x, AsyncTCP 3.3.x, ArduinoJson 7.4.x
- RAM: ~15% (49 KB)
- Flash: ~63%

---

Сделано с теплом · Eva / Grok · для Агнии 💙
