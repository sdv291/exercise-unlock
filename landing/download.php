<?php
// Exercise Unlock: the personal download link from the email.
//
//   GET  download.php?t=<token>   a page with the download button (mail scanners and link previews open links
//                                 too, so opening one never spends a download)
//   POST download.php  t=<token>  takes one download and streams the installer
//
// A link works link_days days and link_downloads downloads; a spent or unknown one gets a page that sends the
// visitor back to the form. Every download is logged with the address the link was made for.

declare(strict_types=1);
require __DIR__ . '/lib.php';

header('X-Content-Type-Options: nosniff');
header('Referrer-Policy: no-referrer');   // the token is in this page's address: never pass it on (the fonts)
header('X-Robots-Tag: noindex');
header('Cache-Control: no-store');

$isPost = ($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'POST';
$token = (string)($isPost ? ($_POST['t'] ?? '') : ($_GET['t'] ?? ''));
$ticket = $isPost ? '' : (string)($_GET['d'] ?? '');
$ip = client_ip();

if ($ticket !== '') {
    // the file itself, for the ticket the button's POST gave (any number of GETs while it lasts)
    $tic = is_file($CFG['installer_path']) ? ticket_check($CFG, $ticket) : null;
    if ($tic) {
        if (!preg_match('/^bytes=[1-9]/', (string)($_SERVER['HTTP_RANGE'] ?? ''))) {
            log_line($CFG, "fetch {$tic['email']} ip=$ip link={$tic['link']}");
        }
        if ($CFG['accel_uri'] !== '') {
            // nginx serves the file (ranges, resume); it keeps this Content-Type and Content-Disposition
            header('Content-Type: application/octet-stream');
            header('Content-Disposition: attachment; filename="' . $CFG['installer_name'] . '"');
            header('X-Accel-Redirect: ' . $CFG['accel_uri']);
            exit;
        }
        send_file($CFG['installer_path'], $CFG['installer_name']);
        exit;
    }
    log_line($CFG, "fetch-refused ip=$ip");
    $rec = null;
} elseif ($isPost) {
    // the button: spend one download, then send the browser to a plain GET address for the file
    $rec = is_file($CFG['installer_path']) ? token_check($CFG, $token, true, $ip) : null;
    $newTicket = $rec ? ticket_new($CFG, $rec, $token) : null;
    if ($newTicket !== null) {
        log_line($CFG, "download {$rec['email']} ip=$ip left={$rec['left']} link=" . substr(hash('sha256', $token), 0, 12));
        header('Location: download.php?d=' . $newTicket, true, 303);
        exit;
    }
    log_line($CFG, "download-refused ip=$ip link=" . (token_valid_shape($token) ? substr(hash('sha256', $token), 0, 12) : 'malformed'));
    $rec = null;
} else {
    $rec = token_check($CFG, $token, false);
}

$lang = pick_language($STR, $rec['lang'] ?? (isset($_GET['lang']) ? (string)$_GET['lang'] : null));
$T = $STR[$lang] ?? [];
if (!$rec) http_response_code($isPost ? 410 : 404);
header('Content-Type: text/html; charset=utf-8');
$home = './?lang=' . $lang;
?><!doctype html>
<html lang="<?= h($lang) ?>" data-type="software" data-radius="round" data-buttons="pill">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex">
<title><?= h($rec ? $T['dl_title'] : $T['dl_bad_title']) ?></title>
<link rel="icon" href="assets/img/logo.svg" type="image/svg+xml">
<meta name="theme-color" content="#121633">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Manrope:wght@400..700&display=swap" rel="stylesheet">
<link rel="stylesheet" href="assets/system.css">
<style>
:root {
  --p-bg: #121633; --p-surface: #1b2050; --p-ink: #eef0ff; --p-muted: #a7add8;
  --p-accent: #2bff88; --p-accent-ink: #121633; --p-action: #2bff88; --p-action-ink: #121633;
  --p-inv-bg: #eef0ff; --p-inv-surface: #dcdfef; --p-inv-ink: #121633; --p-inv-muted: #5b617b;
  --p-inv-accent: #121633; --p-inv-accent-ink: #eef0ff; --p-inv-action: #121633; --p-inv-action-ink: #eef0ff;
  --font-display: "Manrope", system-ui, sans-serif;
  --font-text: "Manrope", system-ui, sans-serif;
  --display-weight: 500;
}
body { min-height: 100svh; display: grid; place-items: center; padding: var(--s-6) var(--gut);
  background: radial-gradient(120% 90% at 80% 0%, #2a62c9 0%, #1b3a8a 30%, #121633 70%); }
.eu-dl { width: min(100%, 520px); display: grid; gap: var(--s-5); padding: var(--s-7); border-radius: var(--r-lg);
  background: rgba(13, 17, 52, .82); border: 1px solid rgba(255, 255, 255, .14); }
.eu-dl h1 { font-size: clamp(32px, 6vw, 44px); line-height: 1.1; margin: 0; }
.eu-dl p { margin: 0; }
.eu-dl .nav__logo img { width: 28px; height: 28px; border-radius: 8px; }
.eu-dl form .btn, .eu-dl > .btn { width: 100%; }
@media (max-width: 639px) { .eu-dl { padding: var(--s-5); } }
</style>
</head>
<body>
<main class="eu-dl">
  <a class="nav__logo" href="<?= h($home) ?>"><img src="assets/img/logo.svg" alt="" width="28" height="28">Exercise Unlock</a>
<?php if ($rec): ?>
  <h1><?= h($T['dl_title']) ?></h1>
  <p class="t-lead"><?= h($T['dl_lead']) ?></p>
  <form method="post" action="download.php">
    <input type="hidden" name="t" value="<?= h($token) ?>">
    <button class="btn btn--primary" type="submit"><?= h($T['dl_button']) ?></button>
  </form>
  <p class="t-small muted"><?= h(sprintf($T['dl_left'], (int)$rec['left'], date('d.m.Y', (int)$rec['expires']))) ?></p>
<?php else: ?>
  <h1><?= h($T['dl_bad_title']) ?></h1>
  <p class="t-lead"><?= h($T['dl_bad_lead']) ?></p>
  <a class="btn btn--primary" href="<?= h($home . '&link=expired#get') ?>"><?= h($T['dl_bad_button']) ?></a>
<?php endif; ?>
</main>
</body>
</html>
