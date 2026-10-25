<?php

namespace App\Windows;

use RageKit\Window;

/**
 * The main window's frame, drawn by the app instead of the OS: a back button when there's a screen
 * to go back to, the screen's title, and the minimise/maximise/close buttons. Dragging the bar
 * moves the window. Its template is templates/Windows/AppWindow.rk.
 */
final class AppWindow extends Window
{
    public static function template(): string
    {
        return 'templates/Windows/AppWindow.rk';
    }

    public static function context(): string
    {
        return TitleBar::class;
    }
}
