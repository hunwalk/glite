<?php

namespace App\Screens;

use App\Windows\AppWindow;
use App\Windows\TitleBar;
use Logi\Device;
use Logi\Logi;
use RageKit\Screen;

/**
 * Sets a LIGHTSYNC keyboard's keys to one colour: a swatch, or red/green/blue sliders. The colour
 * stays until the keyboard is replugged, G HUB takes over or "Give back" hands the lighting back
 * to the keyboard; nothing is saved on it. Its template is templates/Screens/LightingScreen.rk.
 */
final class LightingScreen extends Screen implements TitleBar
{
    /** The swatches, in order; each name is also its style (a colour must be literal in STYLES). */
    public const SWATCHES = ['White', 'Red', 'Orange', 'Yellow', 'Green', 'Cyan', 'Blue', 'Purple', 'Pink'];
    public const COLORS = [
        'White' => 0xFFFFFF, 'Red' => 0xFF0000, 'Orange' => 0xFF5000, 'Yellow' => 0xFFC800, 'Green' => 0x00FF00,
        'Cyan' => 0x00FFFF, 'Blue' => 0x0040FF, 'Purple' => 0x8000FF, 'Pink' => 0xFF0080,
    ];

    public const STYLES = [
        'swatches' => ['flexDirection' => 'row', 'flexWrap' => 'wrap', 'gap' => 12],
        'swatch' => ['width' => 88, 'gap' => 6],
        // width: an empty View isn't stretched across its column View (it would be 0 wide)
        'chip' => ['width' => 88, 'height' => 40,'borderRadius' => 8, 'borderWidth' => 1, 'borderColor' => '#3f3f46'],
        'picked' => ['borderWidth' => 3, 'borderColor' => '#fafafa'],
        'White' => ['backgroundColor' => '#ffffff'],
        'Red' => ['backgroundColor' => '#ff0000'],
        'Orange' => ['backgroundColor' => '#ff5000'],
        'Yellow' => ['backgroundColor' => '#ffc800'],
        'Green' => ['backgroundColor' => '#00ff00'],
        'Cyan' => ['backgroundColor' => '#00ffff'],
        'Blue' => ['backgroundColor' => '#0040ff'],
        'Purple' => ['backgroundColor' => '#8000ff'],
        'Pink' => ['backgroundColor' => '#ff0080'],
        'channel' => ['width' => 48],
    ];

    public string $status = '';
    /** the swatch last sent; '' for a custom colour or none yet */
    public string $picked = '';
    public float $red = 255.0;
    public float $green = 255.0;
    public float $blue = 255.0;
    private bool $busy = false;

    public function __construct(private ?Device $device = null)
    {
    }

    public static function window(): string
    {
        return AppWindow::class;
    }

    public function title(): string
    {
        return $this->device !== null ? $this->device->name . ' lighting' : 'Lighting';
    }

    public function pick(string $swatch): void
    {
        $color = self::COLORS[$swatch] ?? -1;
        if ($color < 0) {
            return;
        }
        $this->red = (float) (($color >> 16) & 0xFF);
        $this->green = (float) (($color >> 8) & 0xFF);
        $this->blue = (float) ($color & 0xFF);
        if ($this->paint($color, $swatch)) {
            $this->picked = $swatch;
        }
    }

    public function applyCustom(): void
    {
        if ($this->paint($this->custom(), $this->customHex())) {
            $this->picked = '';
        }
    }

    /** The sliders' colour as #RRGGBB. */
    public function customHex(): string
    {
        return '#' . strtoupper(str_pad(dechex($this->custom()), 6, '0', STR_PAD_LEFT));
    }

    public function giveBack(): void
    {
        if ($this->device === null || $this->busy) {
            return;
        }
        $this->busy = true;
        $started = Logi::giveBack($this->device, function (string $error): void {
            $this->busy = false;
            $this->picked = '';
            $this->status = $error !== '' ? $error : 'The keyboard shows its own lighting again.';
        });
        if (!$started) {
            $this->busy = false;
            $this->status = 'Busy talking to the devices; try again in a moment.';
        }
    }

    private function custom(): int
    {
        return ($this->channel($this->red) << 16) | ($this->channel($this->green) << 8) | $this->channel($this->blue);
    }

    private function channel(float $value): int
    {
        return max(0, min(255, (int) round($value)));
    }

    /** Sends a colour; false when it couldn't start (no device, or another job is running). */
    private function paint(int $color, string $what): bool
    {
        if ($this->device === null) {
            $this->status = 'No keyboard; go back and scan again.';
            return false;
        }
        if ($this->busy) {
            return false;
        }
        $this->busy = true;
        $started = Logi::light($this->device, $color, function (string $error) use ($what): void {
            $this->busy = false;
            $this->status = $error !== '' ? $error : 'Keys set to ' . $what . '.';
        });
        if (!$started) {
            $this->busy = false;
            $this->status = 'Busy talking to the devices; try again in a moment.';
            return false;
        }
        $this->status = 'Sending ' . $what . '…';
        return true;
    }
}
