<?php

namespace App\Services;

use RageKit\Http\Http;
use RageKit\Http\Response;

/**
 * Device pictures from Logitech's website: its product search (www.logitech.com/…/api/search)
 * finds the product, and Logitech's image CDN (resource.logitechg.com) sends the picture already
 * shrunk to the size asked for. The app ships no pictures; ragekit caches the downloaded ones.
 *
 * Devices are matched by name, so a device the shop doesn't sell (any more) may get none.
 */
final class LogitechImages
{
    private const SEARCH = 'https://www.logitech.com/en-eu/api/search?q=';
    private const CDN = 'https://resource.logitechg.com/';

    /** @var array<string, string> device name => picture URL ('' = none found) */
    private array $found = [];

    /**
     * Calls $then(string $url) with a URL of the device's picture fitting $width x $height pixels,
     * or '' when the shop has none (or can't be reached). Runs on the UI thread, later.
     */
    public function find(string $name, int $width, int $height, \Closure $then): void
    {
        if (isset($this->found[$name])) {
            $then($this->found[$name]);
            return;
        }
        // "G522 LIGHTSPEED - Wireless Mode" finds nothing; "G522 LIGHTSPEED" does.
        $parts = explode(' - ', $name);
        $query = trim($parts[0]);
        Http::get(self::SEARCH . rawurlencode($query))
            ->header('Accept', 'application/json')
            ->timeout(10)
            ->send(function (Response $r) use ($name, $width, $height, $then): void {
                $path = $r->ok() ? self::imagePath($name, $r->body) : '';
                $url = $path === '' ? '' : self::CDN . 'w_' . $width . ',h_' . $height . ',c_fit,q_auto,f_png' . $path;
                if ($r->ok()) {
                    $this->found[$name] = $url; // failures are tried again next time
                }
                $then($url);
            });
    }

    /** The product picture's path ("/content/dam/…png") for the best match, or ''. */
    private static function imagePath(string $name, string $body): string
    {
        $data = json_decode($body, true);
        if (!is_array($data) || !is_array($data['hits'] ?? null)) {
            return '';
        }
        $wanted = self::slug($name);
        $best = null;
        $bestScore = 0;
        foreach ($data['hits'] as $hit) {
            $product = $hit['baseData'] ?? null;
            if (!is_array($product)) {
                continue;
            }
            $title = self::slug((string) ($product['title'] ?? ''));
            $score = 0;
            if (self::slug((string) ($product['productIdentifier'] ?? '')) === $wanted) {
                $score = 3000;
            } elseif ($title !== '' && $title === $wanted) {
                $score = 2000;
            } elseif ($title !== '' && str_starts_with($wanted, $title . '-')) {
                $score = 1000 + strlen($title);
            }
            if ($score > $bestScore) {
                $best = $product;
                $bestScore = $score;
            }
        }
        if ($best === null || !is_array($best['variants'] ?? null)) {
            return '';
        }
        // Colour variants: the device doesn't say which one it is; most are black.
        $first = '';
        foreach ($best['variants'] as $variant) {
            $path = (string) ($variant['productImages'][0]['path'] ?? '');
            if ($path === '' || !str_starts_with($path, '/')) {
                continue;
            }
            if (str_contains($path, 'black')) {
                return $path;
            }
            if ($first === '') {
                $first = $path;
            }
        }
        return $first;
    }

    /** "G502 HERO Gaming Mouse" => "g502-hero-gaming-mouse" */
    private static function slug(string $text): string
    {
        $slug = '';
        $gap = false;
        foreach (str_split(strtolower($text)) as $c) {
            if (($c >= 'a' && $c <= 'z') || ($c >= '0' && $c <= '9')) {
                if ($gap && $slug !== '') {
                    $slug .= '-';
                }
                $slug .= $c;
                $gap = false;
            } else {
                $gap = true;
            }
        }
        return $slug;
    }
}
