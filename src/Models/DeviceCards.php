<?php

namespace App\Models;

use RageKit\Collection;

/** The home screen's cards, one per device found. */
final class DeviceCards extends Collection
{
    public const OF = DeviceCard::class;
}
