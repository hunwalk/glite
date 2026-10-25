<?php

namespace App\Stores;

use RageKit\Store;

/** A store: public properties are app-wide state, methods change them or compute from them. */
final class Profile extends Store
{
    public string $name = '';

    public function greeting(): string
    {
        return $this->name === '' ? 'Type your name above' : 'Hello, ' . $this->name . '!';
    }
}
