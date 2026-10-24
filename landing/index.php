<?php
// Exercise Unlock — landing page with the download-link form.
//
// One file: GETs render the page, POSTs validate + send the download
// link by email + re-render with a success/error banner. Two UI
// languages: Ukrainian when the browser asks for it, English otherwise
// (?lang=xx and the language switch override that, kept in a cookie).
//
// Settings (download URL, sender, SMTP) live OUTSIDE the web folder in
// ../landing-config.php (see landing-config.example.php), so updating
// this page never overwrites them and the SMTP password is never in a
// public directory. Mail goes over SMTP when smtp_host is set, else via
// PHP mail().

// ============================================================
// Settings
// ============================================================
$CFG = [
    'download_url'   => 'https://example.com/downloads/ExerciseUnlock-Setup.exe',
    'mail_from'      => 'noreply@example.com',
    'mail_from_name' => 'Exercise Unlock',
    'smtp_host'      => '',        // e.g. smtp.gmail.com; empty = PHP mail()
    'smtp_port'      => 587,
    'smtp_secure'    => 'tls',     // 'tls' = STARTTLS (587), 'ssl' = implicit TLS (465)
    'smtp_user'      => '',
    'smtp_pass'      => '',
    'log_file'       => dirname(__DIR__) . '/landing-requests.log',   // outside the web folder
    'github_url'     => 'https://github.com/sdv291/exercise-unlock',
];
$localConfig = dirname(__DIR__) . '/landing-config.php';
if (is_file($localConfig)) {
    $over = include $localConfig;
    if (is_array($over)) $CFG = array_merge($CFG, $over);
}

const COOKIE_LANG = 'eu_lang';
// Anti-abuse: the form mails whatever address it is given.
const LIMIT_IP_SEC      = 30;    // one request per IP per 30 s
const LIMIT_IP_PER_HOUR = 5;     // ... and at most 5 per hour
const LIMIT_EMAIL_SEC   = 600;   // the same address at most every 10 min

// The app icon (branding/app-icon.svg).
const APP_ICON_SVG = <<<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><defs><linearGradient id="eu-bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2e3a86"/><stop offset="1" stop-color="#151a3e"/></linearGradient><linearGradient id="eu-bell" x1="0.2" y1="0" x2="0.8" y2="1"><stop offset="0" stop-color="#7dffbc"/><stop offset="0.55" stop-color="#2bff88"/><stop offset="1" stop-color="#12c98a"/></linearGradient><linearGradient id="eu-handle" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#d9e1ff"/><stop offset="1" stop-color="#ffffff"/></linearGradient><radialGradient id="eu-glow" cx="0.5" cy="0.66" r="0.5"><stop offset="0" stop-color="#2bff88" stop-opacity="0.30"/><stop offset="1" stop-color="#2bff88" stop-opacity="0"/></radialGradient></defs><rect x="8" y="8" width="240" height="240" rx="56" fill="url(#eu-bg)"/><rect x="8" y="8" width="240" height="240" rx="56" fill="url(#eu-glow)"/><rect x="9.5" y="9.5" width="237" height="237" rx="54.5" fill="none" stroke="#ffffff" stroke-opacity="0.10" stroke-width="3"/><g transform="translate(4 3)"><path transform="rotate(-16 84 124)" d="M82 132 V94 Q82 62 114 62 H142 Q174 62 174 94 V96" fill="none" stroke="url(#eu-handle)" stroke-width="22" stroke-linecap="round" stroke-linejoin="round"/><path d="M98 223 A72 72 0 1 1 158 223 Z" fill="url(#eu-bell)"/><ellipse cx="92" cy="138" rx="20" ry="11" transform="rotate(-42 92 138)" fill="#ffffff" fill-opacity="0.30"/><g fill="#1a2050" stroke="#1a2050" stroke-linecap="round" stroke-linejoin="round"><circle cx="128" cy="142" r="12.5" stroke="none"/><path d="M121 163 L135 163 L143 198 Q144 203 139 203 L117 203 Q112 203 113 198 Z" stroke="none"/><path d="M123 167 L103 145 M133 167 L153 145" fill="none" stroke-width="10"/></g></g></svg>
SVG;

// ============================================================
// Translations. Add a language by adding a top-level key with
// every string below.
// ============================================================
$LANGS = [
    'uk' => 'Українська',
    'en' => 'English',
];

$STR = [
    'uk' => [
        'html_lang'   => 'uk',
        'title'       => 'Exercise Unlock — комп\'ютер відкривається вправами',
        'meta_desc'   => 'Застосунок для Windows: щоб увійти до свого облікового запису, дитина робить присідання й віджимання перед вебкамерою або телефоном. Кожен повтор додає хвилини за комп\'ютером.',
        'tag'         => 'Windows 10 · 11',
        'h1_line1'    => 'Комп\'ютер відкривається',
        'h1_line2'    => 'вправами',
        'lead'        => 'На екрані входу Windows у дитини з\'являється плитка Exercise Unlock. Присідання й віджимання перед вебкамерою або телефоном перетворюються на хвилини за комп\'ютером, а коли час закінчується, Windows блокується сама.',
        'email_label' => 'Email',
        'email_ph'    => 'you@example.com',
        'button'      => 'Отримати посилання',
        'form_note'   => 'Надішлемо посилання на інсталятор. Для встановлення потрібні права адміністратора.',
        'ok'          => 'Посилання надіслано на %s. Перевірте пошту, зокрема теку «Спам».',
        'err_email'   => 'Введіть коректний email.',
        'err_rate'    => 'Забагато спроб. Зачекайте трохи й спробуйте знову.',
        'err_send'    => 'Не вдалося надіслати листа. Спробуйте пізніше.',
        'ok_silent'   => 'Посилання надіслано. Перевірте пошту.',
        'how_t'       => 'Як це працює',
        'step1_t'     => 'Встановіть',
        'step1_d'     => 'Інсталятор запитає, для якого облікового запису дитини ввімкнути Exercise Unlock, і його пароль Windows. Пароль зберігається зашифрованим — прочитати його може лише система.',
        'step2_t'     => 'Відкалібруйте',
        'step2_d'     => 'Чотири фото — стоячи, у присіді, у планці й унизу віджимання — і по 5 пробних повторів. Якщо пробні повтори не зараховуються, калібрування саме підправить пороги або попросить перезняти фото.',
        'step3_t'     => 'Дитина тренується',
        'step3_d'     => 'На екрані блокування кожен зарахований повтор позначається звуком. Набрано мінімальний час — можна входити, і таймер пішов.',
        'feat_t'      => 'Можливості',
        'feat1_t'     => 'Присідання й віджимання',
        'feat1_d'     => 'Поза розпізнається нейромережею просто на комп\'ютері. Підійде звичайна вебкамера.',
        'feat2_t'     => 'Телефон замість камери',
        'feat2_d'     => 'QR-код на екрані блокування відкриває сторінку на телефоні, і повтори рахує він. Працює, коли комп\'ютер має інтернет; без нього — вебкамера.',
        'feat3_t'     => 'Хвилини за повтори',
        'feat3_d'     => 'Типово присід — 3 хвилини, віджимання — 6. Увійти можна від 10 хвилин, за один раз — не більше 60. Усе налаштовується.',
        'feat4_t'     => 'Розклад',
        'feat4_d'     => 'Повтори зараховуються лише в дозволені години (типово 07:00–20:00), будні й вихідні — окремо.',
        'feat5_t'     => 'Автоблокування',
        'feat5_d'     => 'Перед кінцем часу — попередження, наприкінці Windows блокується. Якщо дитина заблокувала комп\'ютер раніше, залишок зберігається.',
        'feat6_t'     => 'Захист від обходу',
        'feat6_d'     => 'Служба працює від імені системи — дитина не може її зупинити. Віртуальні камери й зациклене відео не зараховуються.',
        'feat7_t'     => 'Звуки',
        'feat7_d'     => 'Сигнал «бачу тебе», звук на кожен повтор і звук помилки, якщо повтор не додає часу: поза розкладом або коли ліміт уже набрано.',
        'feat8_t'     => 'Під вашу дитину',
        'feat8_d'     => 'Майстер калібрування з прикладами поз; пороги, нагороди й розклад — в одному config.ini.',
        'privacy_t'   => 'Відео нікуди не йде',
        'privacy_d'   => 'Зображення з вебкамери обробляється лише на цьому комп\'ютері. На телефоні поза рахується в браузері, а на комп\'ютер через наш сервер передаються лише дані про повтори — без картинки.',
        'req_t'       => 'Що потрібно',
        'req1'        => 'Windows 10 або 11, 64-біт',
        'req2'        => 'Вебкамера або смартфон із браузером',
        'req3'        => 'Окремий обліковий запис Windows для дитини',
        'req4'        => 'Права адміністратора для встановлення',
        'footer'      => 'Вихідний код на GitHub',
        'mail_subject'=> 'Посилання на завантаження Exercise Unlock',
        'mail_body'   => "Вітаємо!\n\nВи запросили посилання на Exercise Unlock.\n\nЗавантажити інсталятор:\n%s\n\nВстановлення потребує прав адміністратора: воно реєструє службу й плитку на екрані входу Windows. Після встановлення відкриється майстер калібрування.\n\nВихідний код і опис: %s\n\n— Exercise Unlock\n",
    ],
    'en' => [
        'html_lang'   => 'en',
        'title'       => 'Exercise Unlock — the PC unlocks with exercise',
        'meta_desc'   => 'A Windows app: to sign in, the child does squats and push-ups in front of the webcam or a phone. Every rep adds minutes of computer time.',
        'tag'         => 'Windows 10 · 11',
        'h1_line1'    => 'The PC unlocks',
        'h1_line2'    => 'with exercise',
        'lead'        => 'An Exercise Unlock tile appears on the child\'s Windows sign-in screen. Squats and push-ups in front of the webcam or a phone turn into minutes of computer time, and when the time is up, Windows locks itself.',
        'email_label' => 'Email',
        'email_ph'    => 'you@example.com',
        'button'      => 'Get the link',
        'form_note'   => 'We\'ll email you the installer link. Installing needs administrator rights.',
        'ok'          => 'Link sent to %s. Check your inbox, and the Spam folder too.',
        'err_email'   => 'Please enter a valid email.',
        'err_rate'    => 'Too many attempts. Please wait a moment and try again.',
        'err_send'    => 'Could not send the email. Please try again later.',
        'ok_silent'   => 'Link sent. Please check your inbox.',
        'how_t'       => 'How it works',
        'step1_t'     => 'Install',
        'step1_d'     => 'Setup asks which child account to protect and its Windows password. The password is stored encrypted — only the system can read it.',
        'step2_t'     => 'Calibrate',
        'step2_d'     => 'Four photos — standing, squatting, plank and push-up bottom — then 5 test reps each. If test reps don\'t count, calibration adjusts the thresholds itself or asks for new photos.',
        'step3_t'     => 'The child exercises',
        'step3_d'     => 'On the lock screen every counted rep plays a sound. Once the minimum time is earned the child signs in and the timer starts.',
        'feat_t'      => 'Features',
        'feat1_t'     => 'Squats and push-ups',
        'feat1_d'     => 'Pose recognition runs right on the PC. Any ordinary webcam will do.',
        'feat2_t'     => 'A phone as the camera',
        'feat2_d'     => 'A QR code on the lock screen opens a page on the phone, and the phone counts the reps. Works while the PC is online; offline it\'s the webcam.',
        'feat3_t'     => 'Minutes for reps',
        'feat3_d'     => 'By default a squat earns 3 minutes and a push-up 6. Sign-in from 10 minutes, at most 60 at a time. All adjustable.',
        'feat4_t'     => 'Schedule',
        'feat4_d'     => 'Reps count only during allowed hours (07:00–20:00 by default), with separate weekday and weekend hours.',
        'feat5_t'     => 'Auto-lock',
        'feat5_d'     => 'A warning before the time runs out, then Windows locks. If the child locks the PC early, the rest is saved.',
        'feat6_t'     => 'Hard to cheat',
        'feat6_d'     => 'The service runs as the system — the child can\'t stop it. Virtual cameras and looped video don\'t count.',
        'feat7_t'     => 'Sounds',
        'feat7_d'     => 'A "ready" cue, a sound for every rep, and an error sound when a rep adds no time: outside the schedule or with the limit reached.',
        'feat8_t'     => 'Tuned to your child',
        'feat8_d'     => 'A calibration wizard with example poses; thresholds, rewards and schedule in one config.ini.',
        'privacy_t'   => 'Video stays where it is',
        'privacy_d'   => 'The webcam picture is processed on this PC only. On a phone the pose is worked out in the browser, and only rep data — no images — reaches the PC through our server.',
        'req_t'       => 'Requirements',
        'req1'        => 'Windows 10 or 11, 64-bit',
        'req2'        => 'A webcam or a smartphone with a browser',
        'req3'        => 'A separate Windows account for the child',
        'req4'        => 'Administrator rights to install',
        'footer'      => 'Source code on GitHub',
        'mail_subject'=> 'Your Exercise Unlock download link',
        'mail_body'   => "Hello!\n\nYou asked for a link to Exercise Unlock.\n\nDownload the installer:\n%s\n\nInstalling needs administrator rights: it registers a service and a tile on the Windows sign-in screen. The calibration wizard opens when setup finishes.\n\nSource code and docs: %s\n\n— Exercise Unlock\n",
    ],
];

// ============================================================
// Language resolution.
// ============================================================
function pick_language(array $langs): string
{
    // Explicit ?lang=xx wins and is persisted in a cookie.
    if (isset($_GET['lang']) && isset($langs[$_GET['lang']])) {
        $lang = $_GET['lang'];
        setcookie(COOKIE_LANG, $lang, [
            'expires'  => time() + 365 * 24 * 3600,
            'path'     => '/',
            'httponly' => true,
            'samesite' => 'Lax',
        ]);
        return $lang;
    }
    // Sticky cookie from an earlier visit.
    if (isset($_COOKIE[COOKIE_LANG]) && isset($langs[$_COOKIE[COOKIE_LANG]])) {
        return $_COOKIE[COOKIE_LANG];
    }
    // The browser's language: the first tag in Accept-Language we can
    // serve, English when none is (e.g. a Russian or German browser).
    $accept = $_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? '';
    foreach (explode(',', $accept) as $tag) {
        $code = strtolower(substr(trim($tag), 0, 2));
        // Common alias: some browsers may still emit `ua` for Ukraine.
        if ($code === 'ua') $code = 'uk';
        if (isset($langs[$code])) return $code;
    }
    return 'en';
}

$lang = pick_language($LANGS);
$T = $STR[$lang];

function t(string $key): string
{
    global $T;
    return $T[$key] ?? $key;
}

function h(string $s): string
{
    return htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
}

// ============================================================
// POST handling.
// ============================================================
$state = 'idle';   // idle | ok | error
$message = '';
$emailValue = '';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $emailRaw = trim((string)($_POST['email'] ?? ''));
    $emailValue = $emailRaw;
    $ip = $_SERVER['REMOTE_ADDR'] ?? 'unknown';

    if (!empty($_POST['website'])) {
        // Honey-pot triggered — silently "succeed".
        $state = 'ok';
        $message = h(t('ok_silent'));
    } elseif ($emailRaw === '' || strlen($emailRaw) > 254 || !filter_var($emailRaw, FILTER_VALIDATE_EMAIL)) {
        $state = 'error';
        $message = h(t('err_email'));
    } elseif (!rate_limit_ok($ip, strtolower($emailRaw))) {
        $state = 'error';
        $message = h(t('err_rate'));
    } elseif (send_download_link($CFG, $emailRaw, $T)) {
        log_request($CFG, $emailRaw, $ip, 'sent', $lang);
        $state = 'ok';
        $message = sprintf(h(t('ok')), h($emailRaw));
        $emailValue = '';
    } else {
        log_request($CFG, $emailRaw, $ip, 'send-failed', $lang);
        $state = 'error';
        $message = h(t('err_send'));
    }
}

// Per-IP and per-address throttle kept in one small JSON file in the
// system temp dir (a session cookie alone is trivially dropped).
function rate_limit_ok(string $ip, string $email): bool
{
    $file = sys_get_temp_dir() . '/exercise-unlock-landing-rl.json';
    $fh = @fopen($file, 'c+');
    if (!$fh) return true;   // can't throttle - don't block real users
    flock($fh, LOCK_EX);
    $data = json_decode(stream_get_contents($fh) ?: '{}', true) ?: [];
    $now = time();
    $ipKey = 'ip:' . hash('sha256', $ip);
    $mailKey = 'mail:' . hash('sha256', $email);
    // Forget anything older than an hour.
    foreach ($data as $k => $v) {
        $data[$k] = array_values(array_filter((array)$v, fn($ts) => $now - $ts < 3600));
        if (!$data[$k]) unset($data[$k]);
    }
    $ipHits = $data[$ipKey] ?? [];
    $mailHits = $data[$mailKey] ?? [];
    $ok = (!$ipHits || $now - max($ipHits) >= LIMIT_IP_SEC)
       && count($ipHits) < LIMIT_IP_PER_HOUR
       && (!$mailHits || $now - max($mailHits) >= LIMIT_EMAIL_SEC);
    if ($ok) {
        $data[$ipKey][] = $now;
        $data[$mailKey][] = $now;
    }
    ftruncate($fh, 0);
    rewind($fh);
    fwrite($fh, json_encode($data));
    flock($fh, LOCK_UN);
    fclose($fh);
    return $ok;
}

function send_download_link(array $cfg, string $to, array $T): bool
{
    // A placeholder link must never go out.
    if (strpos($cfg['download_url'], 'example.com') !== false) return false;
    $subject = $T['mail_subject'];
    $body = sprintf($T['mail_body'], $cfg['download_url'], $cfg['github_url']);
    return $cfg['smtp_host'] !== ''
        ? smtp_send($cfg, $to, $subject, $body)
        : php_mail_send($cfg, $to, $subject, $body);
}

function encoded_subject(string $subject): string
{
    return '=?UTF-8?B?' . base64_encode($subject) . '?=';
}

function php_mail_send(array $cfg, string $to, string $subject, string $body): bool
{
    $headers = [
        'From: ' . encoded_subject($cfg['mail_from_name']) . ' <' . $cfg['mail_from'] . '>',
        'MIME-Version: 1.0',
        'Content-Type: text/plain; charset=UTF-8',
        'Content-Transfer-Encoding: base64',
    ];
    return @mail($to, encoded_subject($subject), chunk_split(base64_encode($body)), implode("\r\n", $headers));
}

// Minimal SMTP client: STARTTLS or implicit TLS, AUTH LOGIN, one
// plain-text message (base64 body, so no dot-stuffing needed).
function smtp_send(array $cfg, string $to, string $subject, string $body): bool
{
    $implicit = $cfg['smtp_secure'] === 'ssl';
    $ctx = stream_context_create(['ssl' => ['verify_peer' => true, 'verify_peer_name' => true]]);
    $s = @stream_socket_client(($implicit ? 'ssl://' : 'tcp://') . $cfg['smtp_host'] . ':' . (int)$cfg['smtp_port'],
                               $errno, $errstr, 15, STREAM_CLIENT_CONNECT, $ctx);
    if (!$s) return false;
    stream_set_timeout($s, 15);

    $expect = function (array $codes) use ($s): bool {
        $reply = '';
        while (($line = fgets($s, 1024)) !== false) {
            $reply .= $line;
            if (strlen($line) < 4 || $line[3] === ' ') break;   // last line of the reply
        }
        return in_array((int)substr($reply, 0, 3), $codes, true);
    };
    $cmd = function (string $line, array $codes) use ($s, $expect): bool {
        fwrite($s, $line . "\r\n");
        return $expect($codes);
    };

    $host = preg_replace('/[^A-Za-z0-9.-]/', '', $_SERVER['SERVER_NAME'] ?? 'localhost') ?: 'localhost';
    $ok = $expect([220]) && $cmd("EHLO $host", [250]);
    if ($ok && !$implicit && $cfg['smtp_secure'] === 'tls') {
        $crypto = STREAM_CRYPTO_METHOD_TLSv1_2_CLIENT |
                  (defined('STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT') ? STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT : 0);
        $ok = $cmd('STARTTLS', [220]) && stream_socket_enable_crypto($s, true, $crypto) && $cmd("EHLO $host", [250]);
    }
    if ($ok && $cfg['smtp_user'] !== '') {
        $ok = $cmd('AUTH LOGIN', [334])
           && $cmd(base64_encode($cfg['smtp_user']), [334])
           && $cmd(base64_encode($cfg['smtp_pass']), [235]);
    }
    $ok = $ok && $cmd('MAIL FROM:<' . $cfg['mail_from'] . '>', [250])
              && $cmd('RCPT TO:<' . $to . '>', [250, 251])
              && $cmd('DATA', [354]);
    if ($ok) {
        $domain = substr(strrchr($cfg['mail_from'], '@') ?: '@localhost', 1);
        $message = implode("\r\n", [
            'From: ' . encoded_subject($cfg['mail_from_name']) . ' <' . $cfg['mail_from'] . '>',
            'To: <' . $to . '>',
            'Subject: ' . encoded_subject($subject),
            'Date: ' . date('r'),
            'Message-ID: <' . bin2hex(random_bytes(12)) . '@' . $domain . '>',
            'MIME-Version: 1.0',
            'Content-Type: text/plain; charset=UTF-8',
            'Content-Transfer-Encoding: base64',
            '',
            rtrim(chunk_split(base64_encode($body), 76, "\r\n")),
            '.',
        ]);
        $ok = $cmd($message, [250]);
    }
    @$cmd('QUIT', [221]);
    fclose($s);
    return $ok;
}

function log_request(array $cfg, string $email, string $ip, string $result, string $lang): void
{
    $line = sprintf("[%s] %s  ip=%s  lang=%s  result=%s\n", date('Y-m-d H:i:s'), $email, $ip, $lang, $result);
    @file_put_contents($cfg['log_file'], $line, FILE_APPEND | LOCK_EX);
}

// Build the "?lang=xx" URLs preserving other query params.
function lang_url(string $target): string
{
    $q = $_GET;
    $q['lang'] = $target;
    return '?' . http_build_query($q);
}

$favicon = 'data:image/svg+xml;base64,' . base64_encode(APP_ICON_SVG);
?>
<!doctype html>
<html lang="<?= h(t('html_lang')) ?>">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title><?= h(t('title')) ?></title>
<meta name="description" content="<?= h(t('meta_desc')) ?>">
<meta name="theme-color" content="#121633">
<link rel="icon" type="image/svg+xml" href="<?= h($favicon) ?>">
<meta property="og:title" content="<?= h(t('title')) ?>">
<meta property="og:description" content="<?= h(t('meta_desc')) ?>">
<style>
:root {
    --bg: #0d1029;
    --panel: #161a3a;
    --panel-2: #1d2248;
    --border: #2b3166;
    --text: #eef0ff;
    --muted: #9aa0c8;
    --accent: #2bff88;
    --accent-2: #12c98a;
    --accent-shadow: rgba(43, 255, 136, 0.22);
    --ok: #2bff88;
    --err: #ff7b8a;
}

* { box-sizing: border-box; }

html, body {
    margin: 0;
    padding: 0;
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    -webkit-font-smoothing: antialiased;
}

body {
    background:
        radial-gradient(1100px 620px at 50% -12%, rgba(43,255,136,0.10), transparent 60%),
        radial-gradient(900px 520px at 100% 20%, rgba(46,58,134,0.55), transparent 60%),
        var(--bg);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
}

a { color: var(--accent); }

header {
    padding: 20px 40px;
    display: flex;
    align-items: center;
    gap: 12px;
}

.logo { width: 40px; height: 40px; flex: none; }
.logo, .hero-icon { display: block; }

.brand { font-size: 18px; font-weight: 650; letter-spacing: 0.2px; flex: 1; }

.lang-switch {
    display: flex;
    gap: 4px;
    padding: 4px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 999px;
}
.lang-switch a {
    padding: 6px 12px;
    border-radius: 999px;
    color: var(--muted);
    text-decoration: none;
    font-size: 12px;
    font-weight: 650;
    letter-spacing: 0.4px;
    text-transform: uppercase;
}
.lang-switch a:hover { color: var(--text); }
.lang-switch a.active { background: var(--panel-2); color: var(--accent); }

main { flex: 1; padding: 24px 24px 72px; }

.wrap { max-width: 980px; margin: 0 auto; }

.hero { max-width: 660px; margin: 0 auto; text-align: center; padding-top: 16px; }

.hero-icon {
    width: 112px;
    height: 112px;
    margin: 0 auto 22px;
    filter: drop-shadow(0 18px 40px rgba(43, 255, 136, 0.18));
}

.tag {
    display: inline-block;
    padding: 6px 12px;
    border-radius: 999px;
    background: var(--panel);
    color: var(--muted);
    font-size: 12px;
    letter-spacing: 0.8px;
    text-transform: uppercase;
    border: 1px solid var(--border);
    margin-bottom: 18px;
}

h1 {
    font-size: 50px;
    line-height: 1.05;
    margin: 0 0 18px;
    font-weight: 750;
    letter-spacing: -0.02em;
}
h1 .accent {
    background: linear-gradient(90deg, #7dffbc, var(--accent), var(--accent-2));
    -webkit-background-clip: text;
    background-clip: text;
    color: transparent;
}

.lead {
    color: var(--muted);
    font-size: 17px;
    line-height: 1.6;
    margin: 0 auto 32px;
    max-width: 580px;
}

.card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 18px;
    padding: 22px;
    text-align: left;
    box-shadow: 0 30px 80px rgba(0, 0, 0, 0.35);
}

form { display: flex; gap: 10px; flex-wrap: wrap; }

label.field { flex: 1 1 260px; display: block; min-width: 220px; }
label.field span {
    display: block;
    font-size: 12px;
    color: var(--muted);
    margin-bottom: 6px;
    letter-spacing: 0.4px;
    text-transform: uppercase;
}

input[type=email] {
    width: 100%;
    padding: 14px 16px;
    background: var(--panel-2);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 12px;
    font-size: 16px;
    outline: none;
    transition: border-color .15s ease, box-shadow .15s ease;
}
input[type=email]:focus { border-color: var(--accent); box-shadow: 0 0 0 4px var(--accent-shadow); }

button {
    align-self: flex-end;
    padding: 14px 24px;
    background: linear-gradient(180deg, var(--accent), var(--accent-2));
    color: #0b2a18;
    border: 0;
    border-radius: 12px;
    font-size: 15px;
    font-weight: 750;
    cursor: pointer;
    letter-spacing: 0.2px;
    transition: transform .05s ease, filter .15s ease;
    box-shadow: 0 10px 24px -8px var(--accent-shadow);
}
button:hover { filter: brightness(1.05); }
button:active { transform: translateY(1px); }
button:focus-visible { outline: 3px solid #fff; outline-offset: 2px; }

.form-note { margin: 12px 2px 0; font-size: 13px; color: var(--muted); }

.hp { position: absolute; left: -9999px; top: -9999px; }

.banner {
    margin: 0 0 16px;
    padding: 12px 14px;
    border-radius: 12px;
    font-size: 14px;
    line-height: 1.5;
    border: 1px solid var(--border);
    background: var(--panel-2);
}
.banner.ok  { border-color: rgba(43,255,136,0.5); color: var(--ok); }
.banner.err { border-color: rgba(255,123,138,0.5); color: var(--err); }

section.block { margin-top: 72px; }
h2 { font-size: 28px; margin: 0 0 22px; letter-spacing: -0.01em; text-align: center; }

.steps { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; }
.step {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 20px;
}
.step-num {
    width: 32px;
    height: 32px;
    border-radius: 10px;
    display: grid;
    place-items: center;
    font-weight: 800;
    color: #0b2a18;
    background: linear-gradient(180deg, var(--accent), var(--accent-2));
    margin-bottom: 12px;
}
.step h3, .feature h3 { margin: 0 0 6px; font-size: 16px; }
.step p, .feature p { margin: 0; color: var(--muted); font-size: 14px; line-height: 1.55; }

.features { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; }
.feature {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 14px;
    padding: 16px;
}
.feature h3 { font-size: 15px; }
.feature p { font-size: 13px; }

.split { display: grid; grid-template-columns: 1.3fr 1fr; gap: 14px; }
.note {
    background: linear-gradient(135deg, rgba(43,255,136,0.10), rgba(46,58,134,0.25));
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 22px;
}
.note h2 { text-align: left; font-size: 22px; margin-bottom: 10px; }
.note p { margin: 0; color: var(--muted); line-height: 1.6; }
.note ul { margin: 0; padding: 0; list-style: none; }
.note li { color: var(--muted); padding: 6px 0 6px 26px; position: relative; line-height: 1.45; }
.note li::before {
    content: "";
    position: absolute;
    left: 2px;
    top: 12px;
    width: 10px;
    height: 10px;
    border-radius: 3px;
    background: var(--accent);
}

footer {
    padding: 22px 40px;
    color: var(--muted);
    font-size: 13px;
    text-align: center;
    border-top: 1px solid var(--border);
}

@media (max-width: 900px) {
    .features { grid-template-columns: repeat(2, 1fr); }
    .steps, .split { grid-template-columns: 1fr; }
}
@media (max-width: 640px) {
    header { padding: 14px 16px; }
    h1 { font-size: 34px; }
    .lead { font-size: 15px; }
    main { padding: 12px 16px 48px; }
    button { width: 100%; }
    label.field { flex: 1 1 100%; min-width: 0; }
    .features { grid-template-columns: 1fr; }
    section.block { margin-top: 52px; }
    footer { padding: 18px 16px; }
}
</style>
</head>
<body>
<header>
    <img class="logo" src="<?= h($favicon) ?>" alt="" width="40" height="40">
    <div class="brand">Exercise Unlock</div>
    <nav class="lang-switch" aria-label="Language">
        <?php foreach ($LANGS as $code => $name): ?>
            <a href="<?= h(lang_url($code)) ?>"
               class="<?= $code === $lang ? 'active' : '' ?>"
               hreflang="<?= h($code) ?>" title="<?= h($name) ?>"><?= h(strtoupper($code)) ?></a>
        <?php endforeach; ?>
    </nav>
</header>

<main>
    <div class="wrap">
        <section class="hero">
            <img class="hero-icon" src="<?= h($favicon) ?>" alt="" width="112" height="112">
            <span class="tag"><?= h(t('tag')) ?></span>
            <h1><?= h(t('h1_line1')) ?><br><span class="accent"><?= h(t('h1_line2')) ?></span></h1>
            <p class="lead"><?= h(t('lead')) ?></p>

            <div class="card" id="get">
                <?php if ($state !== 'idle'): ?>
                    <div class="banner <?= $state === 'ok' ? 'ok' : 'err' ?>" role="status">
                        <?= $message /* escaped above */ ?>
                    </div>
                <?php endif; ?>

                <form method="post" action="#get" novalidate>
                    <label class="field">
                        <span><?= h(t('email_label')) ?></span>
                        <input type="email" name="email" required
                               placeholder="<?= h(t('email_ph')) ?>"
                               value="<?= h($emailValue) ?>"
                               autocomplete="email" spellcheck="false">
                    </label>

                    <input class="hp" type="text" name="website" tabindex="-1" autocomplete="off" aria-hidden="true">

                    <button type="submit"><?= h(t('button')) ?></button>
                </form>
                <p class="form-note"><?= h(t('form_note')) ?></p>
            </div>
        </section>

        <section class="block">
            <h2><?= h(t('how_t')) ?></h2>
            <div class="steps">
                <?php foreach ([1, 2, 3] as $i): ?>
                    <div class="step">
                        <div class="step-num"><?= $i ?></div>
                        <h3><?= h(t("step{$i}_t")) ?></h3>
                        <p><?= h(t("step{$i}_d")) ?></p>
                    </div>
                <?php endforeach; ?>
            </div>
        </section>

        <section class="block">
            <h2><?= h(t('feat_t')) ?></h2>
            <div class="features">
                <?php foreach (range(1, 8) as $i): ?>
                    <div class="feature">
                        <h3><?= h(t("feat{$i}_t")) ?></h3>
                        <p><?= h(t("feat{$i}_d")) ?></p>
                    </div>
                <?php endforeach; ?>
            </div>
        </section>

        <section class="block split">
            <div class="note">
                <h2><?= h(t('privacy_t')) ?></h2>
                <p><?= h(t('privacy_d')) ?></p>
            </div>
            <div class="note">
                <h2><?= h(t('req_t')) ?></h2>
                <ul>
                    <?php foreach ([1, 2, 3, 4] as $i): ?>
                        <li><?= h(t("req{$i}")) ?></li>
                    <?php endforeach; ?>
                </ul>
            </div>
        </section>
    </div>
</main>

<footer>
    © <?= date('Y') ?> Exercise Unlock · <a href="<?= h($CFG['github_url']) ?>"><?= h(t('footer')) ?></a>
</footer>
</body>
</html>
