<?php

namespace Logi;

/** A device a scan found (see Logi::scan()). */
final class Device
{
    public function __construct(
        public string $name,
        /** keyboard, mouse, headset, …; 'device' when it doesn't say */
        public string $kind,
        /** "cable", "USB Receiver, slot 1", … */
        public string $connection,
        /** percent left; -1 when it has no battery or doesn't say */
        public int $battery,
        /** discharging, charging, full, …; '' when unknown */
        public string $batteryStatus,
        /** has LIGHTSYNC per-key lighting that Logi::light() can set */
        public bool $lighting = false,
        /** where Logi::light() finds it; '' for devices it can't reach */
        public string $address = '',
    ) {
    }

    public function hasBattery(): bool
    {
        return $this->battery >= 0;
    }
}
