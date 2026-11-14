<?php
// Exercise Unlock landing: what index.php and download.php share.
//
// Settings live OUTSIDE the web folder in ../landing-config.php (see landing-config.example.php), so updating the
// page never overwrites them and the SMTP password is never in a public directory. The page texts come from
// strings.json (built by src/build.mjs together with page.uk.html and page.en.html).

declare(strict_types=1);

$CFG = [
    // The page's own address: personal links are <site_url>download.php?t=<token>.
    'site_url'       => 'https://exercise-unlock.sdv291.com/landing/',
    // The installer download.php streams (the same file the updater gets from /download/).
    'installer_path' => '/var/www/exercise-unlock/download/ExerciseUnlock-Setup.exe',
    'installer_name' => 'ExerciseUnlock-Setup.exe',
    // nginx hands out the file itself (X-Accel-Redirect to this address of the same installer; nginx then does
    // byte ranges and resumes). Empty: PHP streams it (a 48 MB stream through PHP-FPM broke off on the VPS).
    'accel_uri'      => '/download/ExerciseUnlock-Setup.exe',
    // A personal link works this many days and this many downloads.
    'link_days'      => 7,
    'link_downloads' => 3,
    'mail_from'      => 'noreply@example.com',
    'mail_from_name' => 'Exercise Unlock',
    'smtp_host'      => '',        // e.g. smtp.gmail.com; empty = PHP mail()
    'smtp_port'      => 587,
    'smtp_secure'    => 'tls',     // 'tls' = STARTTLS (587), 'ssl' = implicit TLS (465)
    'smtp_user'      => '',
    'smtp_pass'      => '',
    // Outside the web folder, writable by the PHP-FPM user (www-data).
    'log_file'       => dirname(__DIR__) . '/landing-requests.log',
    'tokens_file'    => dirname(__DIR__) . '/landing-tokens.json',
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
const LIMIT_IP_PER_HOUR = 10;    // ... and at most 10 per hour
const LIMIT_EMAIL_SEC   = 600;   // the same address at most every 10 min
// Personal links are forgotten this long after they expire (the logs keep who asked and who downloaded).
const TOKEN_KEEP_SEC    = 30 * 24 * 3600;

$STR = json_decode((string)@file_get_contents(__DIR__ . '/strings.json'), true) ?: [];

function h(string $s): string
{
    return htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
}

// ?lang=xx (kept in a cookie), then the cookie, then the browser's Accept-Language; English when none fits.
function pick_language(array $langs, ?string $asked = null): string
{
    if ($asked !== null && isset($langs[$asked])) {
        setcookie(COOKIE_LANG, $asked, [
            'expires'  => time() + 365 * 24 * 3600,
            'path'     => '/',
            'httponly' => true,
            'samesite' => 'Lax',
        ]);
        return $asked;
    }
    if (isset($_COOKIE[COOKIE_LANG]) && isset($langs[$_COOKIE[COOKIE_LANG]])) {
        return $_COOKIE[COOKIE_LANG];
    }
    foreach (explode(',', $_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? '') as $tag) {
        $code = strtolower(substr(trim($tag), 0, 2));
        if ($code === 'ua') $code = 'uk';   // some browsers still send ua for Ukraine
        if (isset($langs[$code])) return $code;
    }
    return 'en';
}

function client_ip(): string
{
    return (string)($_SERVER['REMOTE_ADDR'] ?? 'unknown');
}

function log_line(array $cfg, string $line): void
{
    @file_put_contents($cfg['log_file'], '[' . date('Y-m-d H:i:s') . '] ' . $line . "\n", FILE_APPEND | LOCK_EX);
}

// ------------------------------------------------------------------ a JSON file read and written under one lock

function with_json_file(string $file, callable $fn)
{
    $fh = @fopen($file, 'c+');
    if (!$fh) return $fn(null);
    flock($fh, LOCK_EX);
    $data = json_decode(stream_get_contents($fh) ?: '{}', true) ?: [];
    [$data, $result] = $fn($data);
    ftruncate($fh, 0);
    rewind($fh);
    fwrite($fh, json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES));
    fflush($fh);
    flock($fh, LOCK_UN);
    fclose($fh);
    return $result;
}

// Per-IP and per-address throttle in the system temp dir (a session cookie alone is trivially dropped).
// '' when the request may go, 'email' when this address had a link a moment ago, 'ip' when this network asks
// too often.
function rate_limit(string $ip, string $email): string
{
    $file = sys_get_temp_dir() . '/exercise-unlock-landing-rl.json';
    return with_json_file($file, function ($data) use ($ip, $email) {
        if ($data === null) return '';   // can't throttle - don't block real users
        $now = time();
        $ipKey = 'ip:' . hash('sha256', $ip);
        $mailKey = 'mail:' . hash('sha256', $email);
        foreach ($data as $k => $v) {
            $data[$k] = array_values(array_filter((array)$v, fn($ts) => $now - $ts < 3600));
            if (!$data[$k]) unset($data[$k]);
        }
        $ipHits = $data[$ipKey] ?? [];
        $mailHits = $data[$mailKey] ?? [];
        $why = '';
        if ($mailHits && $now - max($mailHits) < LIMIT_EMAIL_SEC) $why = 'email';
        elseif (($ipHits && $now - max($ipHits) < LIMIT_IP_SEC) || count($ipHits) >= LIMIT_IP_PER_HOUR) $why = 'ip';
        if ($why === '') {
            $data[$ipKey][] = $now;
            $data[$mailKey][] = $now;
        }
        return [$data, $why];
    });
}

// ------------------------------------------------------------------ personal links
// The token itself is only in the email; the store keeps its SHA-256, so the file never holds a working link.

function token_new(array $cfg, string $email, string $lang, string $ip): ?string
{
    $token = rtrim(strtr(base64_encode(random_bytes(18)), '+/', '-_'), '=');   // 24 characters
    $now = time();
    $ok = with_json_file($cfg['tokens_file'], function ($data) use ($cfg, $token, $email, $lang, $ip, $now) {
        if ($data === null) return false;
        foreach ($data as $k => $v) {
            if (($v['expires'] ?? 0) + TOKEN_KEEP_SEC < $now) unset($data[$k]);
        }
        $data[hash('sha256', $token)] = [
            'email'   => $email,
            'lang'    => $lang,
            'ip'      => $ip,
            'created' => $now,
            'expires' => $now + (int)$cfg['link_days'] * 86400,
            'left'    => (int)$cfg['link_downloads'],
            'got'     => [],
        ];
        return [$data, true];
    });
    return $ok ? $token : null;
}

function token_valid_shape(string $token): bool
{
    return (bool)preg_match('/^[A-Za-z0-9_-]{24}$/', $token);
}

// The link's record when it still works (not expired, downloads left), else null. $use takes one download.
function token_check(array $cfg, string $token, bool $use, string $ip = ''): ?array
{
    if (!token_valid_shape($token)) return null;
    $key = hash('sha256', $token);
    $now = time();
    return with_json_file($cfg['tokens_file'], function ($data) use ($key, $now, $use, $ip) {
        if ($data === null) return null;
        $rec = $data[$key] ?? null;
        if (!$rec || $rec['expires'] < $now || $rec['left'] < 1) return [$data, null];
        if ($use) {
            $rec['left']--;
            $rec['got'][] = ['at' => $now, 'ip' => $ip];
            $data[$key] = $rec;
        }
        return [$data, $rec];
    });
}

// A download ticket: the button's POST spends one download of the link and redirects to download.php?d=<ticket>,
// a plain GET address for TICKET_SEC seconds. The browser, its resume and a download-manager extension can fetch
// it as often as they need (they re-request a download with GET, without the form's POST body).
const TICKET_SEC = 15 * 60;

function ticket_new(array $cfg, array $rec, string $token): ?string
{
    $ticket = rtrim(strtr(base64_encode(random_bytes(18)), '+/', '-_'), '=');
    $now = time();
    $ok = with_json_file($cfg['tokens_file'], function ($data) use ($ticket, $rec, $token, $now) {
        if ($data === null) return false;
        foreach ($data as $k => $v) {
            if (!empty($v['ticket']) && $v['expires'] < $now) unset($data[$k]);
        }
        $data['d:' . hash('sha256', $ticket)] = [
            'ticket'  => true,
            'email'   => $rec['email'],
            'link'    => substr(hash('sha256', $token), 0, 12),
            'expires' => $now + TICKET_SEC,
        ];
        return [$data, true];
    });
    return $ok ? $ticket : null;
}

function ticket_check(array $cfg, string $ticket): ?array
{
    if (!token_valid_shape($ticket)) return null;
    $key = 'd:' . hash('sha256', $ticket);
    $now = time();
    return with_json_file($cfg['tokens_file'], function ($data) use ($key, $now) {
        if ($data === null) return null;
        $rec = $data[$key] ?? null;
        return [$data, ($rec && $rec['expires'] >= $now) ? $rec : null];
    });
}

// Streams the file with byte ranges (a broken download resumes) and validators (so a browser may resume).
function send_file(string $path, string $name): void
{
    $size = (int)filesize($path);
    $mtime = (int)filemtime($path);
    $start = 0;
    $end = $size - 1;
    $status = 200;
    $range = (string)($_SERVER['HTTP_RANGE'] ?? '');
    if (preg_match('/^bytes=(\d*)-(\d*)$/', $range, $m) && ($m[1] !== '' || $m[2] !== '')) {
        if ($m[1] === '') {
            $start = max(0, $size - (int)$m[2]);            // the last N bytes
        } else {
            $start = (int)$m[1];
            if ($m[2] !== '') $end = min($end, (int)$m[2]);
        }
        if ($start > $end || $start >= $size) {
            http_response_code(416);
            header("Content-Range: bytes */$size");
            exit;
        }
        $status = 206;
    }
    while (ob_get_level()) ob_end_clean();
    @set_time_limit(0);
    http_response_code($status);
    header('Content-Type: application/octet-stream');
    header('Content-Disposition: attachment; filename="' . $name . '"');
    header('Accept-Ranges: bytes');
    header('Last-Modified: ' . gmdate('D, d M Y H:i:s', $mtime) . ' GMT');
    header('ETag: "' . dechex($size) . '-' . dechex($mtime) . '"');
    header('Content-Length: ' . ($end - $start + 1));
    if ($status === 206) header("Content-Range: bytes $start-$end/$size");
    if (($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'HEAD') exit;
    $fh = fopen($path, 'rb');
    fseek($fh, $start);
    $left = $end - $start + 1;
    while ($left > 0 && !feof($fh) && !connection_aborted()) {
        $chunk = fread($fh, (int)min(65536, $left));
        if ($chunk === false || $chunk === '') break;
        echo $chunk;
        $left -= strlen($chunk);
        flush();
    }
    fclose($fh);
}

// ------------------------------------------------------------------ mail

function send_link(array $cfg, string $to, array $T, string $link): bool
{
    $subject = $T['mail_subject'];
    $body = sprintf($T['mail_body'], $link, (int)$cfg['link_days'], (int)$cfg['link_downloads'], $cfg['github_url']);
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

// Minimal SMTP client: STARTTLS or implicit TLS, AUTH LOGIN, one plain-text message (base64 body, so no
// dot-stuffing needed).
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
