<?php

namespace App;

use App\Assets\Images;
use App\Screens\HomeScreen;
use App\Stores\AppStore;
use RageKit\RootStore;
use RageKit\Screen;

/**
 * The app: its state (createStore()), its first screen and its main window. Closing the main
 * window ends the app. start() runs once the window is open: the place to react to store events,
 * e.g. `AppStore::get()->profile->on('signedOut', …)`.
 */
final class MyApp extends \RageKit\App
{
    protected function createStore(): RootStore
    {
        return new AppStore();
    }

    /** Image groups templates show as $assets->images->logo. */
    protected function assets(): array
    {
        return ['images' => Images::class];
    }

    protected function firstScreen(): Screen
    {
        return new HomeScreen();
    }

    protected function title(): string
    {
        return 'glite';
    }

    protected function width(): int
    {
        return 940;
    }

    protected function height(): int
    {
        return 620;
    }
}
