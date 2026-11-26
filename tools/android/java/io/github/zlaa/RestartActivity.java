package io.github.zlaa;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * RestartActivity - starts ZLAA again (the pause menu's Quit to Start Screen: the game can't be unloaded from a
 * running app). It runs in a process of its own (AndroidManifest.xml: ":restart") and has no window: it ends the old
 * process, opens ZLAAActivity in a fresh one, and ends itself.
 */
public class RestartActivity extends Activity {
    static final String PID = "io.github.zlaa.PID";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra(PID, -1);
        if (pid > 0) Process.killProcess(pid);
        Intent i = new Intent(this, ZLAAActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
