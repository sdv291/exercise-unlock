<?php
// Exercise Unlock — download-link landing page.
//
// One file: GETs render the form, POSTs validate + send the download
// link by email + re-render with a success/error banner. UI supports
// three languages (ru default, uk, en). Language is chosen from
// ?lang=xx, a persisted cookie, or the Accept-Language header, in
// that order.
//
// Edit the CONFIG block below to point at your real download URL
// and sender identity. If your PHP host doesn't have mail() working
// out of the box, drop in PHPMailer over SMTP and swap send_mail().

// ============================================================
// CONFIG — edit these
// ============================================================
const DOWNLOAD_URL   = 'https://example.com/downloads/ExerciseUnlock-Setup.exe';
const MAIL_FROM      = 'noreply@example.com';
const MAIL_FROM_NAME = 'Exercise Unlock';
const RATE_LIMIT_SEC = 30;   // per-IP throttle between submissions
const LOG_FILE       = __DIR__ . '/requests.log';
const COOKIE_LANG    = 'eu_lang';
// ============================================================

session_start();

// ============================================================
// Translations. Add a new language by adding a top-level key
// and providing every string below.
// ============================================================
$LANGS = [
    'ru' => 'Русский',
    'uk' => 'Українська',
    'en' => 'English',
];

$STR = [
    'ru' => [
        'html_lang'      => 'ru',
        'title'          => 'Exercise Unlock — разблокировка через упражнения',
        'meta_desc'      => 'Windows приложение: экран блокировки требует от ребёнка сделать упражнения перед камерой прежде чем разблокировать компьютер.',
        'tag'            => 'Windows · v1.0',
        'h1_line1'       => 'Разблокируй компьютер',
        'h1_line2'       => 'через упражнения',
        'lead'           => 'Приложение блокирует Windows sign-in экран и требует от ребёнка приседания или отжимания перед камерой прежде чем разблокировать компьютер. Полностью native, работает с обычной веб-камерой.',
        'email_label'    => 'Email',
        'email_ph'       => 'you@example.com',
        'button'         => 'Получить ссылку',
        'ok'             => 'Ссылка отправлена на %s. Проверьте почту (в том числе папку «Спам»).',
        'err_email'      => 'Введите корректный email.',
        'err_rate'       => 'Слишком часто — подождите пару секунд и попробуйте снова.',
        'err_send'       => 'Не удалось отправить письмо. Попробуйте позже.',
        'ok_silent'      => 'Ссылка отправлена. Проверьте почту.',
        'feat1_t'        => 'Приседы + отжимания',
        'feat1_d'        => 'MoveNet скелетная модель считает репы через веб-камеру.',
        'feat2_t'        => 'Настраиваемая формула',
        'feat2_d'        => 'Один реп = N секунд компьютерного времени, настраивается в config.',
        'feat3_t'        => 'Расписание',
        'feat3_d'        => 'Активное окно (например 07:00–20:00) вне которого счёт не идёт.',
        'feat4_t'        => 'Auto-lock',
        'feat4_d'        => 'По истечении earned time — автоматическая блокировка.',
        'footer'         => 'Native C++ · Windows Credential Provider',
        'mail_subject'   => 'Ссылка на скачивание Exercise Unlock',
        'mail_body'      => "Здравствуйте!\n\nВы запросили ссылку для скачивания Exercise Unlock.\n\nСкачать установщик:\n%s\n\nИнструкция по установке в README проекта.\nПрограмма требует прав администратора для установки службы\nи регистрации Credential Provider.\n\n— Команда Exercise Unlock\n",
    ],
    'uk' => [
        'html_lang'      => 'uk',
        'title'          => 'Exercise Unlock — розблокування через вправи',
        'meta_desc'      => 'Windows-застосунок: екран блокування вимагає від дитини зробити вправи перед камерою, перш ніж розблокувати комп\'ютер.',
        'tag'            => 'Windows · v1.0',
        'h1_line1'       => 'Розблокуй комп\'ютер',
        'h1_line2'       => 'через вправи',
        'lead'           => 'Застосунок блокує Windows sign-in екран і вимагає від дитини присідання чи віджимання перед камерою, перш ніж розблокувати комп\'ютер. Повністю native, працює зі звичайною веб-камерою.',
        'email_label'    => 'Email',
        'email_ph'       => 'you@example.com',
        'button'         => 'Отримати посилання',
        'ok'             => 'Посилання надіслано на %s. Перевірте пошту (зокрема теку «Спам»).',
        'err_email'      => 'Введіть коректний email.',
        'err_rate'       => 'Забагато спроб — зачекайте кілька секунд і спробуйте знову.',
        'err_send'       => 'Не вдалося надіслати листа. Спробуйте пізніше.',
        'ok_silent'      => 'Посилання надіслано. Перевірте пошту.',
        'feat1_t'        => 'Присідання + віджимання',
        'feat1_d'        => 'Скелетна модель MoveNet рахує повторення через веб-камеру.',
        'feat2_t'        => 'Налаштовувана формула',
        'feat2_d'        => 'Одне повторення = N секунд комп\'ютерного часу, задається в config.',
        'feat3_t'        => 'Розклад',
        'feat3_d'        => 'Активне вікно (наприклад 07:00–20:00), поза яким рахунок не йде.',
        'feat4_t'        => 'Auto-lock',
        'feat4_d'        => 'Після завершення earned time — автоматичне блокування.',
        'footer'         => 'Native C++ · Windows Credential Provider',
        'mail_subject'   => 'Посилання на завантаження Exercise Unlock',
        'mail_body'      => "Вітаємо!\n\nВи запросили посилання для завантаження Exercise Unlock.\n\nЗавантажити інсталятор:\n%s\n\nІнструкція зі встановлення у README проекту.\nПрограма вимагає прав адміністратора для встановлення служби\nта реєстрації Credential Provider.\n\n— Команда Exercise Unlock\n",
    ],
    'en' => [
        'html_lang'      => 'en',
        'title'          => 'Exercise Unlock — unlock through exercise',
        'meta_desc'      => 'Windows app: the sign-in screen requires the child to perform exercises in front of the camera before unlocking the computer.',
        'tag'            => 'Windows · v1.0',
        'h1_line1'       => 'Unlock your PC',
        'h1_line2'       => 'through exercise',
        'lead'           => 'The app gates the Windows sign-in screen and requires the child to do squats or push-ups in front of the camera before the PC unlocks. Fully native, works with any standard webcam.',
        'email_label'    => 'Email',
        'email_ph'       => 'you@example.com',
        'button'         => 'Get download link',
        'ok'             => 'Download link sent to %s. Check your inbox (and the Spam folder).',
        'err_email'      => 'Please enter a valid email.',
        'err_rate'       => 'Too many attempts — please wait a few seconds and try again.',
        'err_send'       => 'Could not send the message. Please try again later.',
        'ok_silent'      => 'Link sent. Please check your inbox.',
        'feat1_t'        => 'Squats + push-ups',
        'feat1_d'        => 'MoveNet skeleton model counts reps from the webcam feed.',
        'feat2_t'        => 'Configurable formula',
        'feat2_d'        => 'One rep = N seconds of computer time — adjustable in config.',
        'feat3_t'        => 'Schedule',
        'feat3_d'        => 'Active window (e.g. 07:00–20:00); outside it reps do not count.',
        'feat4_t'        => 'Auto-lock',
        'feat4_d'        => 'When earned time runs out, the PC is locked automatically.',
        'footer'         => 'Native C++ · Windows Credential Provider',
        'mail_subject'   => 'Your Exercise Unlock download link',
        'mail_body'      => "Hello!\n\nYou requested a link to download Exercise Unlock.\n\nInstaller:\n%s\n\nSee the project README for installation instructions.\nThe app requires administrator rights to install the service\nand register the Credential Provider.\n\n— Exercise Unlock team\n",
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
    // Fall back to the first tag in Accept-Language we can serve.
    $accept = $_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? '';
    foreach (explode(',', $accept) as $tag) {
        $code = strtolower(substr(trim($tag), 0, 2));
        // Common alias: some browsers may still emit `ua` for Ukraine.
        if ($code === 'ua') $code = 'uk';
        if (isset($langs[$code])) return $code;
    }
    return 'ru';
}

$lang = pick_language($LANGS);
$T = $STR[$lang];

function t(string $key): string
{
    global $T;
    return $T[$key] ?? $key;
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

    if (!empty($_POST['website'])) {
        // Honey-pot triggered — silently succeed.
        $state = 'ok';
        $message = t('ok_silent');
    } elseif ($emailRaw === '' || !filter_var($emailRaw, FILTER_VALIDATE_EMAIL)) {
        $state = 'error';
        $message = t('err_email');
    } elseif (!rate_limit_ok($_SERVER['REMOTE_ADDR'] ?? 'unknown')) {
        $state = 'error';
        $message = t('err_rate');
    } elseif (send_download_link($emailRaw, $T)) {
        log_request($emailRaw, $_SERVER['REMOTE_ADDR'] ?? '-', 'sent', $lang);
        $state = 'ok';
        $message = sprintf(t('ok'), htmlspecialchars($emailRaw));
        $emailValue = '';
    } else {
        log_request($emailRaw, $_SERVER['REMOTE_ADDR'] ?? '-', 'send-failed', $lang);
        $state = 'error';
        $message = t('err_send');
    }
}

function rate_limit_ok(string $ip): bool
{
    $key = 'rl_' . md5($ip);
    $prev = $_SESSION[$key] ?? 0;
    if (time() - $prev < RATE_LIMIT_SEC) return false;
    $_SESSION[$key] = time();
    return true;
}

function send_download_link(string $to, array $T): bool
{
    $subject = $T['mail_subject'];
    $body    = sprintf($T['mail_body'], DOWNLOAD_URL);

    $headers = [
        'From: ' . MAIL_FROM_NAME . ' <' . MAIL_FROM . '>',
        'Reply-To: ' . MAIL_FROM,
        'X-Mailer: PHP/' . phpversion(),
        'Content-Type: text/plain; charset=UTF-8',
    ];

    return @mail($to, '=?UTF-8?B?' . base64_encode($subject) . '?=',
                 $body, implode("\r\n", $headers));
}

function log_request(string $email, string $ip, string $result, string $lang): void
{
    $line = sprintf(
        "[%s] %s  ip=%s  lang=%s  result=%s\n",
        date('Y-m-d H:i:s'), $email, $ip, $lang, $result
    );
    @file_put_contents(LOG_FILE, $line, FILE_APPEND | LOCK_EX);
}

// Build the "?lang=xx" URLs preserving other query params.
function lang_url(string $target): string
{
    $q = $_GET;
    $q['lang'] = $target;
    return '?' . http_build_query($q);
}
?>
<!doctype html>
<html lang="<?= htmlspecialchars(t('html_lang')) ?>">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title><?= htmlspecialchars(t('title')) ?></title>
<meta name="description" content="<?= htmlspecialchars(t('meta_desc')) ?>">
<style>
:root {
    --bg: #0a0a0b;
    --panel: #131316;
    --panel-2: #1a1a1e;
    --border: #2a2a30;
    --text: #ececef;
    --muted: #8a8a94;
    --accent: #22d3ee;
    --accent-hi: #67e8f9;
    --accent-shadow: rgba(34, 211, 238, 0.22);
    --ok: #34d399;
    --err: #f87171;
}

* { box-sizing: border-box; }

html, body {
    margin: 0;
    padding: 0;
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
                 Helvetica, Arial, sans-serif;
    -webkit-font-smoothing: antialiased;
    min-height: 100%;
}

body {
    background:
        radial-gradient(1200px 600px at 20% -10%, rgba(34,211,238,0.08), transparent 60%),
        radial-gradient(900px 500px at 90% 10%, rgba(139,92,246,0.06), transparent 60%),
        var(--bg);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
}

header {
    padding: 24px 40px;
    display: flex;
    align-items: center;
    gap: 14px;
}

.logo {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: linear-gradient(135deg, var(--accent), #8b5cf6);
    display: grid;
    place-items: center;
    color: #05060a;
    font-weight: 800;
}

.logo svg { width: 22px; height: 22px; }

.brand {
    font-size: 18px;
    font-weight: 600;
    letter-spacing: 0.2px;
    flex: 1;
}

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
    font-weight: 600;
    letter-spacing: 0.4px;
    text-transform: uppercase;
    transition: color .15s ease, background .15s ease;
}

.lang-switch a:hover {
    color: var(--text);
}

.lang-switch a.active {
    background: var(--panel-2);
    color: var(--accent);
}

main {
    flex: 1;
    display: flex;
    justify-content: center;
    align-items: center;
    padding: 40px 24px 80px;
}

.hero {
    max-width: 640px;
    width: 100%;
    text-align: center;
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
    margin-bottom: 22px;
}

h1 {
    font-size: 48px;
    line-height: 1.05;
    margin: 0 0 16px;
    font-weight: 700;
    letter-spacing: -0.02em;
    background: linear-gradient(180deg, #fff, #a1a1aa);
    -webkit-background-clip: text;
    background-clip: text;
    color: transparent;
}

.lead {
    color: var(--muted);
    font-size: 17px;
    line-height: 1.6;
    margin: 0 auto 36px;
    max-width: 520px;
}

.card {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 24px;
    text-align: left;
    box-shadow: 0 30px 80px rgba(0,0,0,0.35);
}

form {
    display: flex;
    gap: 10px;
    flex-wrap: wrap;
}

label.field {
    flex: 1 1 260px;
    display: block;
    min-width: 220px;
}

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
    border-radius: 10px;
    font-size: 16px;
    outline: none;
    transition: border-color .15s ease, box-shadow .15s ease;
}

input[type=email]:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 4px var(--accent-shadow);
}

button {
    align-self: flex-end;
    padding: 14px 22px;
    background: linear-gradient(180deg, var(--accent), #06b6d4);
    color: #05060a;
    border: 0;
    border-radius: 10px;
    font-size: 15px;
    font-weight: 700;
    cursor: pointer;
    letter-spacing: 0.2px;
    transition: transform .05s ease, box-shadow .15s ease, filter .15s ease;
    box-shadow: 0 10px 24px -8px var(--accent-shadow);
}

button:hover  { filter: brightness(1.05); }
button:active { transform: translateY(1px); }

.hp { position: absolute; left: -9999px; top: -9999px; }

.banner {
    margin: 0 0 18px;
    padding: 12px 14px;
    border-radius: 10px;
    font-size: 14px;
    line-height: 1.5;
    border: 1px solid var(--border);
    background: var(--panel-2);
}
.banner.ok  { border-color: rgba(52,211,153,0.5); color: var(--ok); }
.banner.err { border-color: rgba(248,113,113,0.5); color: var(--err); }

.features {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 12px;
    margin-top: 28px;
    text-align: left;
}

.feature {
    background: var(--panel-2);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 14px 16px;
}

.feature-title {
    font-size: 13px;
    color: var(--text);
    font-weight: 600;
    margin-bottom: 4px;
}

.feature-desc {
    font-size: 12px;
    color: var(--muted);
    line-height: 1.5;
}

footer {
    padding: 24px 40px;
    color: var(--muted);
    font-size: 12px;
    text-align: center;
    border-top: 1px solid var(--border);
}

@media (max-width: 640px) {
    header { padding: 16px 16px; flex-wrap: wrap; }
    .brand { flex: 1 1 auto; }
    .lang-switch { order: 3; margin-left: auto; }
    h1 { font-size: 34px; }
    .lead { font-size: 15px; }
    main { padding: 20px 16px 40px; }
    button { width: 100%; }
    label.field { flex: 1 1 100%; }
}
</style>
</head>
<body>
<header>
    <div class="logo" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
            <path d="M6 6l3-2 6 12 3-2"/><path d="M3 10l3-2"/><path d="M18 16l3-2"/>
        </svg>
    </div>
    <div class="brand">Exercise Unlock</div>
    <nav class="lang-switch" aria-label="Language">
        <?php foreach ($LANGS as $code => $name): ?>
            <a href="<?= htmlspecialchars(lang_url($code)) ?>"
               class="<?= $code === $lang ? 'active' : '' ?>"
               hreflang="<?= htmlspecialchars($code) ?>"><?= htmlspecialchars(strtoupper($code)) ?></a>
        <?php endforeach; ?>
    </nav>
</header>

<main>
    <section class="hero">
        <span class="tag"><?= htmlspecialchars(t('tag')) ?></span>
        <h1><?= htmlspecialchars(t('h1_line1')) ?><br><?= htmlspecialchars(t('h1_line2')) ?></h1>
        <p class="lead"><?= htmlspecialchars(t('lead')) ?></p>

        <div class="card">
            <?php if ($state !== 'idle'): ?>
                <div class="banner <?= $state === 'ok' ? 'ok' : 'err' ?>">
                    <?= $message /* already escaped where needed */ ?>
                </div>
            <?php endif; ?>

            <form method="post" novalidate>
                <label class="field">
                    <span><?= htmlspecialchars(t('email_label')) ?></span>
                    <input type="email" name="email" required
                           placeholder="<?= htmlspecialchars(t('email_ph')) ?>"
                           value="<?= htmlspecialchars($emailValue) ?>"
                           autocomplete="email" spellcheck="false">
                </label>

                <input class="hp" type="text" name="website" tabindex="-1" autocomplete="off">

                <button type="submit"><?= htmlspecialchars(t('button')) ?></button>
            </form>
        </div>

        <div class="features">
            <div class="feature">
                <div class="feature-title"><?= htmlspecialchars(t('feat1_t')) ?></div>
                <div class="feature-desc"><?= htmlspecialchars(t('feat1_d')) ?></div>
            </div>
            <div class="feature">
                <div class="feature-title"><?= htmlspecialchars(t('feat2_t')) ?></div>
                <div class="feature-desc"><?= htmlspecialchars(t('feat2_d')) ?></div>
            </div>
            <div class="feature">
                <div class="feature-title"><?= htmlspecialchars(t('feat3_t')) ?></div>
                <div class="feature-desc"><?= htmlspecialchars(t('feat3_d')) ?></div>
            </div>
            <div class="feature">
                <div class="feature-title"><?= htmlspecialchars(t('feat4_t')) ?></div>
                <div class="feature-desc"><?= htmlspecialchars(t('feat4_d')) ?></div>
            </div>
        </div>
    </section>
</main>

<footer>
    © <?= date('Y') ?> Exercise Unlock · <?= htmlspecialchars(t('footer')) ?>
</footer>
</body>
</html>
