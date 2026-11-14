<?php
// Settings for the landing (index.php, download.php, lib.php). Copy this file ONE LEVEL ABOVE the landing folder
// (next to it, not inside it) as landing-config.php:
//
//   /var/www/exercise-unlock/landing/index.php        <- the page
//   /var/www/exercise-unlock/landing-config.php       <- this file, with real values
//   /var/www/exercise-unlock/landing-tokens.json      <- the personal links (made by PHP, writable by www-data)
//   /var/www/exercise-unlock/landing-requests.log     <- who asked and who downloaded (writable by www-data)
//
// It stays out of the web folder, so the SMTP password is never served and updating the page never overwrites
// it. Only keys you set here override the defaults in lib.php.
return [
    // The page's address; personal links are <site_url>download.php?t=<token>.
    'site_url'       => 'https://exercise-unlock.sdv291.com/landing/',

    // The installer download.php streams to a personal link (the same file the updater takes from /download/).
    'installer_path' => '/var/www/exercise-unlock/download/ExerciseUnlock-Setup.exe',
    // nginx gives out the file through this address (X-Accel-Redirect); '' makes PHP stream it instead.
    'accel_uri'      => '/download/ExerciseUnlock-Setup.exe',

    // A personal link works this many days and this many downloads.
    'link_days'      => 7,
    'link_downloads' => 3,

    'mail_from'      => 'noreply@sdv291.com',
    'mail_from_name' => 'Exercise Unlock',

    // SMTP (recommended: plain PHP mail() needs a mail server on the VPS and often lands in spam). Leave
    // smtp_host empty to use mail(). Gmail: smtp.gmail.com, 587, 'tls', the full address as user and an app
    // password (Google account -> Security -> App passwords).
    'smtp_host'      => '',
    'smtp_port'      => 587,
    'smtp_secure'    => 'tls',    // 'tls' = STARTTLS (587), 'ssl' = implicit TLS (465)
    'smtp_user'      => '',
    'smtp_pass'      => '',

    // Both outside the web folder, writable by the PHP-FPM user (www-data).
    // 'log_file'     => '/var/www/exercise-unlock/landing-requests.log',
    // 'tokens_file'  => '/var/www/exercise-unlock/landing-tokens.json',
];
