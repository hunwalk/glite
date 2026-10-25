<?php

namespace App\Screens;

use App\Models\DeviceCard;
use App\Models\DeviceCards;
use App\Services\LogitechImages;
use App\Windows\AppWindow;
use App\Windows\TitleBar;
use Logi\Device;
use Logi\Logi;
use RageKit\Screen;

/**
 * Scans for Logitech devices when it opens and shows each as a card: its picture from
 * Logitech's website, how it's connected, and its battery. Its template is
 * templates/Screens/HomeScreen.rk.
 */
final class HomeScreen extends Screen implements TitleBar
{
    /** A card's picture box; Logitech's CDN sends pictures shrunk to fit it. */
    public const IMAGE_WIDTH = 160;
    public const IMAGE_HEIGHT = 100;

    public const STYLES = [
        'cards' => ['flexDirection' => 'row', 'flexWrap' => 'wrap', 'gap' => 12],
        'card' => ['width' => 440],
    ];

    public string $status = 'Looking for Logitech devices…';
    public bool $scanning = false;
    public DeviceCards $devices;
    private ?LogitechImages $catalog = null;

    public function __construct()
    {
        $this->devices = new DeviceCards();
    }

    public static function window(): string
    {
        return AppWindow::class;
    }

    public function title(): string
    {
        return 'Logitech devices';
    }

    public function shown(): void
    {
        $this->scan();
    }

    public function openLighting(int $card): void
    {
        if ($card >= 0 && $card < count($this->devices)) {
            $this->navigator()->push(new LightingScreen($this->devices->get($card)->device()));
        }
    }

    public function available(): bool
    {
        return Logi::available();
    }

    public function scan(): void
    {
        if ($this->scanning || !Logi::available()) {
            return;
        }
        if ($this->catalog === null) {
            $this->catalog = new LogitechImages();
        }
        $this->devices->clear();
        $started = Logi::scan(function (Device $device): void {
            $card = new DeviceCard($device);
            $this->devices->add($card);
            $this->catalog?->find($device->name, self::IMAGE_WIDTH, self::IMAGE_HEIGHT, function (string $url) use ($card): void {
                // the card may belong to an older scan by now; then nobody sees it
                $card->image = $url;
            });
        }, function (string $error): void {
            $this->scanning = false;
            $count = count($this->devices);
            if ($error !== '') {
                $this->status = $error;
            } elseif ($count === 0) {
                $this->status = 'No Logitech devices found.';
            } else {
                $this->status = 'Found ' . $count . ($count === 1 ? ' device.' : ' devices.');
            }
        });
        if ($started) {
            $this->scanning = true;
            $this->status = 'Looking for Logitech devices…';
        }
    }
}
