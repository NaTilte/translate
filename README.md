# remote-desktop

Трансляция экрана Windows-ПК с удалённым управлением мышью (C++).

- `host/` — программа для ПК: захват экрана (DXGI) → H.264 на видеокарте (NVENC и др.) → WebRTC.
- `web/` — веб-клиент для телефона (GitHub Pages / Telegram Web App).
- Видео идёт напрямую ПК → телефон (WebRTC), GitHub хранит код, собирает `host.exe` и раздаёт страницу.

## Сборка
Локально: Visual Studio → «Открыть папку» → `host`. Нужен vcpkg:
`vcpkg install libdatachannel[srtp]:x64-windows-static-md`

Готовый `host.exe` берётся из Actions → build-host → Artifacts (или из Releases для тегов `v*`).
