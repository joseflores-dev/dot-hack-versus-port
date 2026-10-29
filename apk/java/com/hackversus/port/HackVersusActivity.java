package com.hackversus.port;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

/* SDLActivity with the libraries this port ships -- SDL2 itself and libmain.so, the recompiled title -- and the
 * on-screen pad laid over the game surface. */
public class HackVersusActivity extends SDLActivity {
    /* Extras from SetupActivity: the disc image to read, as a document URI (opened here, the descriptor handed to
     * the native side) or as a plain path. Nothing set = the extracted tree under /sdcard/PS3 or the app folder. */
    public static final String EXTRA_ISO_URI = "iso_uri";
    public static final String EXTRA_ISO_PATH = "iso_path";
    private static ParcelFileDescriptor sIsoPfd;   /* kept open for the life of the process */

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        Intent i = getIntent();
        String uri = i != null ? i.getStringExtra(EXTRA_ISO_URI) : null;
        String path = i != null ? i.getStringExtra(EXTRA_ISO_PATH) : null;
        if (uri != null) {
            try {
                if (sIsoPfd != null) sIsoPfd.close();
                sIsoPfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
                if (sIsoPfd != null) return new String[] { "--iso-fd", String.valueOf(sIsoPfd.getFd()) };
            } catch (Exception e) {
                android.util.Log.e("hackversus", "cannot open the disc image " + uri, e);
            }
        }
        if (path != null) return new String[] { "--iso-path", path };
        return new String[0];
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mLayout != null)
            mLayout.addView(new PadView(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
