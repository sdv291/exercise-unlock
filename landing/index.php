<?php
// Exercise Unlock landing page.
//
// GET serves page.<lang>.html (built by src/build.mjs). POST takes an email, makes a personal download link for that
// address (download.php?t=..., good for link_days days and link_downloads downloads) and mails it. The page's script
// posts with Accept: application/json and shows its own line from the answer's status; without script the browser
// posts the form and gets the page back with a banner. Who asked is logged outside the web folder.

declare(strict_types=1);
require __DIR__ . '/lib.php';

header('X-Content-Type-Options: nosniff');
header('Referrer-Policy: strict-origin-when-cross-origin');

$isPost = ($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'POST';
$asked = $isPost ? (string)($_POST['lang'] ?? '') : (isset($_GET['lang']) ? (string)$_GET['lang'] : null);
$lang = pick_language($STR, $asked !== '' ? $asked : null);
$T = $STR[$lang] ?? [];
$wantsJson = strpos((string)($_SERVER['HTTP_ACCEPT'] ?? ''), 'application/json') !== false;   // (no str_contains: the VPS runs PHP 7.4)

$banner = '';   // the line over the form, for a browser that posted without script
if ($isPost) {
    $email = trim((string)($_POST['email'] ?? ''));
    $ip = client_ip();
    [$status, $kind, $text] = (function () use ($CFG, $T, $email, $ip, $lang): array {
        if (!empty($_POST['website'])) return [200, 'ok', sprintf(h($T['ok_banner']), h($email))];   // the honey-pot: say sent, send nothing
        if ($email === '' || strlen($email) > 254 || !filter_var($email, FILTER_VALIDATE_EMAIL)) return [422, 'err', h($T['err_email'])];
        $limited = rate_limit($ip, strtolower($email));
        if ($limited !== '') {
            log_line($CFG, "request $email ip=$ip lang=$lang result=limited-$limited");
            return [429, 'err', h($T[$limited === 'email' ? 'err_rate_mail' : 'err_rate'])];
        }
        if (!is_file($CFG['installer_path'])) {
            log_line($CFG, "request $email ip=$ip lang=$lang result=no-installer");
            return [503, 'err', h($T['err_send'])];
        }
        $token = token_new($CFG, $email, $lang, $ip);
        if ($token === null) {
            log_line($CFG, "request $email ip=$ip lang=$lang result=token-store-failed");
            return [503, 'err', h($T['err_send'])];
        }
        $link = $CFG['site_url'] . 'download.php?t=' . $token;
        if (!send_link($CFG, $email, $T, $link)) {
            log_line($CFG, "request $email ip=$ip lang=$lang result=send-failed");
            return [502, 'err', h($T['err_send'])];
        }
        log_line($CFG, "request $email ip=$ip lang=$lang result=sent link=" . substr(hash('sha256', $token), 0, 12));
        return [200, 'ok', sprintf(h($T['ok_banner']), h($email))];
    })();

    if ($wantsJson) {
        http_response_code($status);
        header('Content-Type: application/json; charset=utf-8');
        header('Cache-Control: no-store');
        echo json_encode(['ok' => $status === 200, 'message' => html_entity_decode(strip_tags($text), ENT_QUOTES, 'UTF-8')], JSON_UNESCAPED_UNICODE);
        exit;
    }
    http_response_code($status);
    $banner = '<p class="eu-banner eu-banner--' . $kind . '" role="status">' . $text . '</p>';
} elseif (($_GET['link'] ?? '') === 'expired') {
    $banner = '<p class="eu-banner eu-banner--err" role="status">' . h($T['expired_banner']) . '</p>';
}

$page = @file_get_contents(__DIR__ . "/page.$lang.html");
if ($page === false) {
    http_response_code(500);
    exit('page.' . h($lang) . '.html is missing: run node src/build.mjs and upload it');
}
header('Content-Type: text/html; charset=utf-8');
header('Vary: Accept-Language, Cookie');
echo str_replace('<!--form-state-->', $banner, $page);
