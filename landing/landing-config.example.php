<?php
// Settings for landing/index.php. Copy this file ONE LEVEL ABOVE the
// landing folder (next to it, not inside it) as landing-config.php:
//
//   /var/www/exercise-unlock/landing/index.php      <- the page
//   /var/www/exercise-unlock/landing-config.php     <- this file, with real values
//
// It stays out of the web folder, so the SMTP password is never served
// and updating the page never overwrites it. Only keys you set here
// override the defaults in index.php.
return [
    // Where the email sends people. Until this is set, the form refuses
    // to send (a placeholder link must never go out).
    'download_url'   => 'https://exercise-unlock.sdv291.com/download/ExerciseUnlock-Setup.exe',

    'mail_from'      => 'noreply@sdv291.com',
    'mail_from_name' => 'Exercise Unlock',

    // SMTP (recommended: plain PHP mail() needs a mail server on the VPS
    // and often lands in spam). Leave smtp_host empty to use mail().
    // Gmail: smtp.gmail.com, 587, 'tls', the full address as user and an
    // app password (Google account -> Security -> App passwords).
    'smtp_host'      => '',
    'smtp_port'      => 587,
    'smtp_secure'    => 'tls',    // 'tls' = STARTTLS (587), 'ssl' = implicit TLS (465)
    'smtp_user'      => '',
    'smtp_pass'      => '',

    // Who asked for the link (email, IP, result). Outside the web folder;
    // must be writable by the PHP-FPM user (www-data).
    // 'log_file'     => '/var/www/exercise-unlock/landing-requests.log',
];
