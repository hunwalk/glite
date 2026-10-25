<?php

namespace Logi;

use RageKit\Events;
use RageKit\Modules;

/**
 * Logitech devices over HID++ (the logi module).
 *
 *     Logi::scan(function (Device $device): void { … },
 *                function (string $error): void { … });
 */
final class Logi
{
    public static function available(): bool
    {
        return Modules::available('logi');
    }

    /**
     * Looks for devices on a worker thread. $onDevice(Device) runs for each one found, then
     * $onDone(error) once ('' when the scan finished). Both run on the UI thread.
     * Returns false when a scan is already running.
     *
     * @throws \RageKit\ModuleUnavailable when the logi module isn't next to the app
     */
    public static function scan(\Closure $onDevice, \Closure $onDone): bool
    {
        Modules::require('logi');
        $id = \logi_scan();
        if ($id === 0) {
            return false;
        }
        Events::on('logi', $id, 'device', function (string $data) use ($onDevice): void {
            $fields = explode("\t", $data);
            $battery = $fields[3] ?? '-1';
            $onDevice(new Device($fields[0] ?? '', $fields[1] ?? '', $fields[2] ?? '',
                is_numeric($battery) ? (int) $battery : -1, $fields[4] ?? '',
                ($fields[5] ?? '') === '1', $fields[6] ?? ''));
        });
        self::whenDone($id, $onDone);
        return true;
    }

    /**
     * Paints every key of a LIGHTSYNC keyboard ($device->lighting) one colour, 0xRRGGBB, until the
     * keyboard is replugged, G HUB takes over or giveBack() runs. Nothing is saved on the keyboard.
     * $onDone(error) runs on the UI thread ('' when it worked). Returns false while a scan or
     * another change is running.
     *
     * @throws \RageKit\ModuleUnavailable when the logi module isn't next to the app
     */
    public static function light(Device $device, int $color, \Closure $onDone): bool
    {
        return self::lightingJob($device, $color & 0xFFFFFF, $onDone);
    }

    /** Hands the keyboard's lighting back to its own (onboard) effects; like light() otherwise. */
    public static function giveBack(Device $device, \Closure $onDone): bool
    {
        return self::lightingJob($device, -1, $onDone);
    }

    private static function lightingJob(Device $device, int $color, \Closure $onDone): bool
    {
        Modules::require('logi');
        $id = \logi_light($device->address, $color);
        if ($id === 0) {
            return false;
        }
        self::whenDone($id, $onDone);
        return true;
    }

    private static function whenDone(int $id, \Closure $onDone): void
    {
        Events::on('logi', $id, 'done', function (string $data) use ($id, $onDone): void {
            Events::off('logi', $id);
            $onDone($data);
        });
    }
}
