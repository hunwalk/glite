<?php

namespace App\Assets;

/**
 * The app's images: paths relative to the project, registered in MyApp::assets(). Templates show
 * LOGO as {{ $assets->images->logo }}; the build checks the files and embeds them.
 */
final class Images
{
    public const LOGO = 'assets/images/logo.svg';
}
