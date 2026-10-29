package com.hackversus.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;

/* Launcher entry: finds the game before the game starts. In order:
 *
 *   1. The disc image the player picked before (a document URI with a persisted read grant): no permission,
 *      no copy, read in place by isofs.c.
 *   2. An extracted tree in the app's own folder (Android/data/<pkg>/files/PS3/vfs/PS3_GAME): no permission.
 *   3. /sdcard/PS3: an extracted tree (vfs/PS3_GAME) or a *.iso there. Needs "All files access"; if it is
 *      missing the dialog offers the system switch for THIS app.
 *   4. Nothing found: the dialog offers the document picker ("Elegir ISO").
 *
 * HackVersusActivity stays exported, so `am start -n com.hackversus.port/.HackVersusActivity` keeps working
 * (it then takes whatever is under /sdcard/PS3). */
public class SetupActivity extends Activity {
    private static final String PREFS = "setup", KEY_ISO_URI = "iso_uri";
    private static final int REQ_PICK_ISO = 1001;
    private boolean sentToSettings = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        proceedOrAsk();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!sentToSettings) return;
        sentToSettings = false;
        if (sdcardHasData() && Environment.isExternalStorageManager()) startGame(null, sdcardIso()); else finish();
    }

    private File appDataDir() { return new File(getExternalFilesDir(null), "PS3"); }
    private boolean appDirHasData() { return new File(appDataDir(), "vfs/PS3_GAME").isDirectory(); }
    private boolean sdcardHasTree() { return new File("/sdcard/PS3/vfs/PS3_GAME").isDirectory(); }
    private String sdcardIso() {
        File[] all = new File("/sdcard/PS3").listFiles();
        if (all != null) for (File f : all) if (f.isFile() && f.getName().toLowerCase().endsWith(".iso")) return f.getPath();
        return null;
    }
    private boolean sdcardHasData() { return sdcardHasTree() || sdcardIso() != null; }

    /* The remembered image, if its grant still opens. */
    private String savedIsoUri() {
        String u = getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_ISO_URI, null);
        if (u == null) return null;
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(u), "r")) {
            return pfd != null ? u : null;
        } catch (Exception e) {
            return null;
        }
    }

    private void startGame(String isoUri, String isoPath) {
        Intent i = new Intent(this, HackVersusActivity.class);
        if (isoUri != null) i.putExtra(HackVersusActivity.EXTRA_ISO_URI, isoUri);
        else if (isoPath != null && !sdcardHasTree()) i.putExtra(HackVersusActivity.EXTRA_ISO_PATH, isoPath);
        startActivity(i);
        finish();
    }

    private void proceedOrAsk() {
        String uri = savedIsoUri();
        if (uri != null) { startGame(uri, null); return; }
        if (appDirHasData()) { startGame(null, null); return; }
        if (sdcardHasData() && Environment.isExternalStorageManager()) { startGame(null, sdcardIso()); return; }

        AlertDialog.Builder b = new AlertDialog.Builder(this)
            .setTitle(".hack//Versus")
            .setMessage("Elige la imagen del disco (.iso) de tu copia del juego: la app la lee directamente, sin "
                        + "copiarla ni extraerla.\n\n"
                        + "Si ya tienes el juego en /sdcard/PS3 (ISO o carpeta vfs/), la app necesita \"Acceso a "
                        + "todos los archivos\": \"Dar permiso\" abre ese interruptor.")
            .setCancelable(false)
            .setPositiveButton("Elegir ISO", (d, w) -> pickIso())
            .setNegativeButton("Salir", (d, w) -> finish());
        if (sdcardHasData() || !Environment.isExternalStorageManager())
            b.setNeutralButton("Dar permiso", (d, w) -> {
                sentToSettings = true;
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + getPackageName()));
                try { startActivity(i); }
                catch (Exception e) { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            });
        b.show();
    }

    private void pickIso() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try { startActivityForResult(i, REQ_PICK_ISO); }
        catch (Exception e) { Toast.makeText(this, "No hay selector de archivos en este equipo", Toast.LENGTH_LONG).show(); finish(); }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req != REQ_PICK_ISO) return;
        if (result != RESULT_OK || data == null || data.getData() == null) { finish(); return; }
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            /* no persistable grant from this provider: it still works for this run */
        }
        /* A quick sanity check: an ISO9660 image says "CD001" at byte 0x8001. */
        boolean ok = false;
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
             java.io.FileInputStream in = new java.io.FileInputStream(pfd.getFileDescriptor())) {
            byte[] hdr = new byte[0x8006];
            int got = 0; while (got < hdr.length) { int n = in.read(hdr, got, hdr.length - got); if (n < 0) break; got += n; }
            ok = got >= 0x8006 && hdr[0x8001] == 'C' && hdr[0x8002] == 'D' && hdr[0x8003] == '0' && hdr[0x8004] == '0' && hdr[0x8005] == '1';
        } catch (Exception e) { ok = false; }
        if (!ok) {
            Toast.makeText(this, "Ese archivo no es una imagen ISO9660 de PS3", Toast.LENGTH_LONG).show();
            finish(); return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_ISO_URI, uri.toString()).apply();
        startGame(uri.toString(), null);
    }
}
