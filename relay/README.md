# Exercise Unlock — сервер-посредник для камеры телефона

Связывает компьютер (службу Exercise Unlock на экране блокировки) с телефоном,
который отсканировал QR-код на плитке. Ничего не хранит (база данных не нужна):
только пересылает короткие сообщения («настройки упражнений», «+1 присед»,
«заработано 6:00»). Видео с камеры телефона никуда не отправляется — поза
распознаётся прямо в браузере телефона.

```
ПК (служба) ─wss─┐                          ┌─wss─ телефон (/s/<код>)
                 └─► nginx :443 (certbot) ─► relay в Docker, 127.0.0.1:8090
```

## Что нужно на VPS

- nginx + certbot (плагин `python3-certbot-nginx`) и Docker с compose — уже есть.
- DNS: A-запись `exercise-unlock.sdv291.com` → IP этого VPS
  (проверка: `nslookup exercise-unlock.sdv291.com`).
- Свободный локальный порт 8090 (`ss -ltn | grep 8090` ничего не показывает).
  Если занят — см. «Другой порт» ниже.

## Установка

1. **Архив без `node_modules`** (на Windows, в папке проекта):
   ```bash
   tar -czf exercise-unlock-relay.tar.gz --exclude=node_modules --exclude=test relay
   ```
   и скопируйте на сервер:
   ```bash
   scp exercise-unlock-relay.tar.gz user@VPS_IP:/tmp/
   ```
2. **Запуск relay** (на сервере):
   ```bash
   sudo mkdir -p /opt/exercise-unlock && sudo tar -xzf /tmp/exercise-unlock-relay.tar.gz -C /opt/exercise-unlock
   cd /opt/exercise-unlock/relay
   sudo docker compose up -d --build
   curl http://127.0.0.1:8090/healthz
   ```
   Ответ `ok 0` — relay работает.
3. **nginx** (сайты в `/etc/nginx/conf.d/`):
   ```bash
   sudo cp nginx-exercise-unlock.conf /etc/nginx/conf.d/exercise-unlock.conf
   sudo nginx -t && sudo systemctl reload nginx
   ```
4. **Сертификат**:
   ```bash
   sudo certbot --nginx -d exercise-unlock.sdv291.com
   ```
   certbot допишет в тот же `server` строки для 443 и сертификата и сделает
   перенаправление с http. Продлевается он вместе с остальными вашими сайтами.
5. **Проверка**:
   ```bash
   curl https://exercise-unlock.sdv291.com/healthz
   ```
   И на телефоне: `https://exercise-unlock.sdv291.com/demo?demo=squat` → «Почати»
   — должен появиться приседающий скелет и счётчик.

## Обновление

Новый архив → распаковать поверх в `/opt/exercise-unlock` →
`cd /opt/exercise-unlock/relay && sudo docker compose up -d --build`.
nginx и certbot трогать не нужно.

## Лендинг (/landing)

Страница с формой «получить ссылку на установщик» — `landing/index.php`
из репозитория. Отдаёт её nginx через php-fpm, Node-сервер в этом не
участвует.

1. PHP: `sudo apt install php-fpm` (если ещё нет); имя сокета —
   `ls /run/php/` (например `php8.1-fpm.sock`).
2. Файлы — с компьютера, из папки репозитория:
   `scp landing/index.php landing/landing-config.example.php user@VPS_IP:/tmp/`,
   затем на VPS:
   ```bash
   sudo mkdir -p /var/www/exercise-unlock/landing /var/www/exercise-unlock/download
   sudo cp /tmp/index.php /var/www/exercise-unlock/landing/
   sudo cp /tmp/landing-config.example.php /var/www/exercise-unlock/landing-config.php
   sudo touch /var/www/exercise-unlock/landing-requests.log
   sudo chown www-data /var/www/exercise-unlock/landing-requests.log
   sudo chown root:www-data /var/www/exercise-unlock/landing-config.php
   sudo chmod 640 /var/www/exercise-unlock/landing-config.php
   ```
   `landing-config.php` лежит **рядом** с папкой `landing`, а не в ней:
   его никто не скачает, и обновление страницы его не затрёт.
3. В `landing-config.php` впишите ссылку на установщик и почту. Удобнее
   всего SMTP (например Gmail с паролем приложения): обычный `mail()`
   требует почтового сервера на VPS, и письма часто попадают в спам.
   Пока ссылка — заглушка `example.com`, форма ничего не отправляет.
4. Установщик, если раздавать его с этого же сервера:
   `/var/www/exercise-unlock/download/ExerciseUnlock-Setup.exe` →
   `https://exercise-unlock.sdv291.com/download/ExerciseUnlock-Setup.exe`.
5. nginx: блоки `location = /landing`, `location ^~ /landing/` и
   `location ^~ /download/` из `nginx-exercise-unlock.conf` добавьте в
   **существующий** `server { }` этого сайта в
   `/etc/nginx/conf.d/exercise-unlock.conf` — в тот, где `listen 443`
   (его дописал certbot), поставив перед `location /`. Сокет в
   `fastcgi_pass` поменяйте на свой. Затем
   `sudo nginx -t && sudo systemctl reload nginx`.

Обновить страницу — снова скопировать `index.php`, больше ничего.

## Журнал

```bash
cd /opt/exercise-unlock/relay && sudo docker compose logs -f relay
```

Там только «pc ABCD… online/offline». Коды из QR не пишутся нигде: в
`nginx-exercise-unlock.conf` журнал запросов для этого сайта выключен
(`access_log off`), потому что код передаётся в адресе.

## Другой порт

Если 8090 занят, выберите свободный, например 8091:

```bash
sudo RELAY_PORT=8091 docker compose up -d --build
```

и поменяйте `proxy_pass http://127.0.0.1:8090;` в конфиге nginx на 8091.

## Если `nginx -t` ругается

- на `listen [::]:80` — на сервере выключен IPv6: удалите эту строку;
- на `map` — файл подключён не внутри `http { }`: проверьте, что в
  `/etc/nginx/nginx.conf` внутри `http { }` есть `include /etc/nginx/conf.d/*.conf;`
  (так по умолчанию).

## Проверки

`npm install && npm test` (нужен Node 20+): распознавание упражнений и
протокол сервера. Без Node — откройте `test/detectors.html` в браузере.

## Протокол

| Кто → кому | Сообщение |
|---|---|
| ПК → relay | `{t:"hello", pc, key}`, `{t:"offer", token}`, `{t:"to", sid, msg}`, `{t:"ping"}` |
| relay → ПК | `{t:"ready"}`, `{t:"join", sid}`, `{t:"leave", sid}`, `{t:"from", sid, msg}`, `{t:"pong"}` |
| ПК → телефон | `{t:"config", aspect, exercises:[{id, seconds, params}]}`, `{t:"status", counts, earned, min, enough, active}`, `{t:"reject", why}`, `{t:"bye", reason}` |
| телефон → ПК | `{t:"hello", lang, ua}`, `{t:"rep", ex, n, q, dur}` |

Новое упражнение: детектор в `public/exercises.js` (реестр `EXERCISES`) и его
`id` в `ExerciseUnlockService/PhoneLink.cpp` (`CreditRep`, `ConfigJson`).
