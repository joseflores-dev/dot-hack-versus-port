package com.hackversus.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import java.io.File;

/* Launcher entry: decides where the game data is before the game starts.
 *
 *   1. <app files>/PS3/EBOOT.ELF (Android/data/com.hackversus.port/files/PS3) needs no permission at all.
 *   2. /sdcard/PS3/EBOOT.ELF needs "All files access" (MANAGE_EXTERNAL_STORAGE). If it is not granted yet,
 *      explain and open the system page for THIS app, where it is one switch; when the user comes back with
 *      it granted, the game starts.
 *
 * HackVersusActivity itself stays exported, so `am start -n com.hackversus.port/.HackVersusActivity` keeps working. */
public class SetupActivity extends Activity {
    private boolean sentToSettings = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        proceedOrAsk();
    }

    /* Back from the system page: start the game if the switch was turned on, otherwise leave quietly. */
    @Override
    protected void onResume() {
        super.onResume();
        if (!sentToSettings) return;
        sentToSettings = false;
        if (appDirHasData() || Environment.isExternalStorageManager()) startGame(); else finish();
    }

    private boolean appDirHasData() {
        File files = getExternalFilesDir(null);
        return files != null && new File(new File(files, "PS3"), "EBOOT.ELF").canRead();
    }

    private void startGame() {
        startActivity(new Intent(this, HackVersusActivity.class));
        finish();
    }

    private void proceedOrAsk() {
        if (appDirHasData() || Environment.isExternalStorageManager()) { startGame(); return; }
        new AlertDialog.Builder(this)
            .setTitle(".hack//Versus")
            .setMessage("El juego se lee desde /sdcard/PS3 (EBOOT.ELF y vfs/). Para eso la app necesita "
                        + "\"Acceso a todos los archivos\": en la pantalla siguiente activa el interruptor y vuelve.\n\n"
                        + "Alternativa sin permiso: copia EBOOT.ELF y vfs/ a Android/data/com.hackversus.port/files/PS3/.")
            .setCancelable(false)
            .setPositiveButton("Dar permiso", (d, w) -> {
                sentToSettings = true;
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + getPackageName()));
                try { startActivity(i); }
                catch (Exception e) { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            })
            .setNegativeButton("Salir", (d, w) -> finish())
            .show();
    }
}
