<?php

namespace App\Windows;

/** What AppWindow's title bar needs from the screen it shows. */
interface TitleBar
{
    /** Shown in the title bar. */
    public function title(): string;
}
