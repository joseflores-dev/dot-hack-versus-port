package com.hackversus.port;

import android.os.Bundle;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

/* SDLActivity with the libraries this port ships -- SDL2 itself and libmain.so, the recompiled title -- and the
 * on-screen pad laid over the game surface. */
public class HackVersusActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mLayout != null)
            mLayout.addView(new PadView(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
