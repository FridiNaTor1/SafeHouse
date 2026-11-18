package org.safehouse.android;

/**
 * The launcher program without its window, in its own process (":direct"): a pinned shortcut's game
 * (`--play sly2`, like `safehouse --play` on the desktop: the app closes when the game quits) or the Collection Style
 * Launcher (`--collection`). The arguments come in the intent's "arguments" extra (LauncherActivity.getArguments).
 */
public final class DirectActivity extends LauncherActivity { }
