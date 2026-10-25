<?php

use App\MyApp;

/** The program's entry point: run the app until its main window closes. */
function main(): void
{
    (new MyApp())->run();
}
