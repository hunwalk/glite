<?php

namespace App\Stores;

use RageKit\RootStore;

/**
 * The app's state, shared by every screen and window. Each store is a typed public property:
 * templates read it as {{ $store->profile->name }}, PHP as AppStore::get()->profile.
 */
final class AppStore extends RootStore
{
    public Profile $profile;

    public function __construct()
    {
        parent::__construct();
        $this->profile = new Profile();
    }
}
