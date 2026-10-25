<?php

namespace App\Models;

use Logi\Device;

/** One device's card on the home screen: what the template shows of a Device, plus its picture. */
final class DeviceCard
{
    /** Logitech's CDN URL for the picture; '' while looking it up or when there's none. */
    public string $image = '';

    public function __construct(private Device $device)
    {
    }

    public function device(): Device
    {
        return $this->device;
    }

    /** whether its lighting can be set (LightingScreen) */
    public function lighting(): bool
    {
        return $this->device->lighting;
    }

    public function name(): string
    {
        return $this->device->name;
    }

    public function info(): string
    {
        return ucfirst($this->device->kind) . ' · ' . $this->device->connection;
    }

    public function battery(): string
    {
        $device = $this->device;
        if (!$device->hasBattery()) {
            return $device->connection === 'cable' ? 'No battery (wired)' : 'Battery: unknown';
        }
        return 'Battery ' . $device->battery . '%' . ($device->batteryStatus !== '' ? ' · ' . $device->batteryStatus : '');
    }

    /** 0 to 1 for the battery bar; -1 hides it */
    public function level(): float
    {
        return $this->device->hasBattery() ? $this->device->battery / 100 : -1.0;
    }
}
