package io.github.zlaa;

import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;
import android.view.KeyEvent;

import org.json.JSONArray;
import org.json.JSONObject;
import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;

/**
 * ZLAAActivity - SDL's activity, plus what the start screen needs from Android to find the player's game files
 * where they are: a folder picker and "All files access" (the game's 6 GB are read in place, never copied); and
 * updates (GitHub's latest release: the check, and the download + install through Android's installer).
 * The native side (host/main.c) calls these static methods through JNI and polls takePickedFolder().
 */
public class ZLAAActivity extends SDLActivity {
    private static final String TAG = "ZLAA";
    private static final int PICK_FOLDER = 0x4C41;
    private static final String LATEST_RELEASE_API = "https://api.github.com/repos/timbo722/ZLAA/releases/latest";

    /** The folder chosen: a file path; "" when the picker was cancelled or the folder can't be read as a path;
     *  null while there is no answer (yet). */
    private static volatile String picked;

    /** May the app read files anywhere in shared storage? */
    public static boolean hasFilesAccess() {
        return Environment.isExternalStorageManager();
    }

    /** Opens Android's settings page where the player allows "All files access" for this app. */
    public static void requestFilesAccess() {
        mSingleton.runOnUiThread(() -> {
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + mSingleton.getPackageName()));
                mSingleton.startActivity(i);
            } catch (Exception e) {
                mSingleton.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
    }

    /** Opens the folder picker; the answer comes to takePickedFolder(). */
    public static void pickFolder() {
        picked = null;
        mSingleton.runOnUiThread(() -> {
            try {
                mSingleton.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
            } catch (Exception e) {
                Log.e(TAG, "no folder picker", e);
                picked = "";
            }
        });
    }

    /** The picker's answer once (see picked), then null again. */
    public static String takePickedFolder() {
        String p = picked;
        picked = null;
        return p;
    }

    /** The pause menu's Quit to Start Screen: the app starts again. RestartActivity, in a process of its own, ends this
     *  one (the native side also exits right after this) and opens ZLAAActivity anew. Called from the native thread:
     *  startActivity has handed the request to Android when it returns. */
    public static void restartApp() {
        Intent i = new Intent(mSingleton, RestartActivity.class);
        i.putExtra(RestartActivity.PID, android.os.Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        mSingleton.startActivity(i);
    }

    /* ---- updates: GitHub's latest release (the start screen's badge), downloaded and installed in the app ---- */

    /** The newest release's tag ("v0.1.2"); "" when the check failed (offline...); null while there is no answer. */
    private static volatile String latestRelease;
    /** That release's ZLAA.apk: where to download it, its size, its SHA-256 (hex; null if GitHub didn't give one) */
    private static volatile String apkUrl, apkSha256;
    private static volatile long apkSize;

    /** The update in the app: UPDATE_* (the native side's UI_UPDATE_* are the same numbers), bytes, a message */
    static final int UPDATE_IDLE = 0, UPDATE_DOWNLOADING = 1, UPDATE_VERIFYING = 2, UPDATE_INSTALLING = 3,
                     UPDATE_FAILED = 4, UPDATE_PERMISSION = 5;
    private static volatile int updateState = UPDATE_IDLE;
    private static volatile long updateDone;
    private static volatile String updateMessage = "";
    private static volatile boolean updateCancel;
    private static final String INSTALL_RESULT = "io.github.zlaa.INSTALL_RESULT";

    /** Reads GitHub's latest release: its tag, and ZLAA.apk's address, size and SHA-256. Returns the tag. */
    private static String fetchLatest() throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(LATEST_RELEASE_API).openConnection();
        try {
            c.setConnectTimeout(5000);
            c.setReadTimeout(5000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "ZLAA");
            if (c.getResponseCode() != 200) throw new Exception("GitHub answered " + c.getResponseCode());
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0;) body.write(b, 0, n);
            }
            JSONObject release = new JSONObject(body.toString("UTF-8"));
            JSONArray assets = release.optJSONArray("assets");
            for (int i = 0; assets != null && i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                if (!a.optString("name").equals("ZLAA.apk")) continue;
                apkUrl = a.optString("browser_download_url", null);
                apkSize = a.optLong("size", 0);
                String digest = a.optString("digest", "");
                apkSha256 = digest.startsWith("sha256:") ? digest.substring(7) : null;
            }
            return release.optString("tag_name", "");
        } finally {
            c.disconnect();
        }
    }

    /** Asks GitHub for the newest release, in the background (5 s at most); the answer comes to takeLatestRelease(). */
    public static void checkForUpdate() {
        latestRelease = null;
        new Thread(() -> {
            String tag = "";
            try {
                tag = fetchLatest();
            } catch (Exception e) {
                Log.i(TAG, "update check: " + e);
            }
            latestRelease = tag;
        }, "update check").start();
    }

    /** The update check's answer once (see latestRelease), then null again. */
    public static String takeLatestRelease() {
        String t = latestRelease;
        latestRelease = null;
        return t;
    }

    /** "state<TAB>bytes done<TAB>bytes in all<TAB>message" for the update window */
    public static String updateStatus() {
        return updateState + "\t" + updateDone + "\t" + apkSize + "\t" + updateMessage;
    }

    /** Download the latest release's APK and install it. Android must let this app install apps ("Install unknown
     *  apps"): the first time its settings page opens and the player chooses Update again after allowing it. */
    public static void startUpdate() {
        int s = updateState;
        if (s == UPDATE_DOWNLOADING || s == UPDATE_VERIFYING || s == UPDATE_INSTALLING) return;
        if (!mSingleton.getPackageManager().canRequestPackageInstalls()) {
            updateState = UPDATE_PERMISSION;
            updateMessage = "";
            mSingleton.runOnUiThread(() -> {
                try {
                    mSingleton.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                                        Uri.parse("package:" + mSingleton.getPackageName())));
                } catch (Exception e) {
                    Log.e(TAG, "no unknown-apps settings page", e);
                }
            });
            return;
        }
        updateCancel = false;
        updateDone = 0;
        updateMessage = "";
        updateState = UPDATE_DOWNLOADING;
        new Thread(ZLAAActivity::runUpdate, "update").start();
    }

    public static void cancelUpdate() {
        updateCancel = true;
    }

    private static void failUpdate(String message) {
        updateMessage = message;
        updateState = UPDATE_FAILED;
    }

    private static void runUpdate() {
        File apk = new File(mSingleton.getCacheDir(), "update.apk");
        try {
            if (apkUrl == null) fetchLatest();
            if (apkUrl == null) { failUpdate("The latest release has no ZLAA.apk."); return; }
            Log.i(TAG, "update: downloading " + apkUrl + " (" + apkSize + " bytes)");
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            HttpURLConnection c = (HttpURLConnection) new URL(apkUrl).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(20000);
            c.setRequestProperty("User-Agent", "ZLAA");
            try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(apk)) {
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0;) {
                    if (updateCancel) { updateState = UPDATE_IDLE; return; }
                    out.write(b, 0, n);
                    sha.update(b, 0, n);
                    updateDone += n;
                }
            } finally {
                c.disconnect();
            }
            updateState = UPDATE_VERIFYING;
            StringBuilder hex = new StringBuilder();
            for (byte x : sha.digest()) hex.append(String.format("%02x", x));
            if (apkSize > 0 && apk.length() != apkSize) { failUpdate("The download was incomplete."); return; }
            if (apkSha256 != null && !apkSha256.equalsIgnoreCase(hex.toString())) {
                failUpdate("The download was damaged (its checksum doesn't match)."); return;
            }
            Log.i(TAG, "update: downloaded " + apk.length() + " bytes, sha256 " + hex
                       + (apkSha256 != null ? " (matches GitHub's)" : " (GitHub gave none to compare)"));
            // Android's installer: the session takes the APK, then asks the player (STATUS_PENDING_USER_ACTION in
            // onNewIntent); on success Android stops this app and the new version starts fresh
            PackageInstaller installer = mSingleton.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(mSingleton.getPackageName());
            int id = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (OutputStream out = session.openWrite("ZLAA.apk", 0, apk.length());
                     FileInputStream in = new FileInputStream(apk)) {
                    byte[] b = new byte[1 << 16];
                    for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n);
                    session.fsync(out);
                }
                Intent result = new Intent(mSingleton, ZLAAActivity.class).setAction(INSTALL_RESULT);
                PendingIntent pending = PendingIntent.getActivity(mSingleton, 1, result,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                updateState = UPDATE_INSTALLING;
                session.commit(pending.getIntentSender());
            }
        } catch (Exception e) {
            Log.e(TAG, "update failed", e);
            failUpdate("Couldn't download the update. Check the internet connection and try again.");
        } finally {
            apk.delete();
        }
    }

    /** The installer's answer: ask the player (Android's own "Update this app?"), or what went wrong */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent == null || !INSTALL_RESULT.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        Log.i(TAG, "update: installer status " + status + " " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) startActivity(confirm);
        } else if (status == PackageInstaller.STATUS_FAILURE_ABORTED) {
            updateMessage = "";
            updateState = UPDATE_IDLE;                      // (the player chose Cancel)
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            failUpdate("Android didn't install the update.");
        }
    }

    /** Android's Back key. SDL turns a controller's Back key into the controller's Back/Select button, which the
     *  game reads as its Minus button (so Back opened the game's own menu). Sent as the system Back key instead,
     *  it opens our pause menu during the game and goes back inside our menus (host/main.c menu_action). */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && !SDLActivity.mBrokenLibraries) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0)
                SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_BACK);
            else if (event.getAction() == KeyEvent.ACTION_UP)
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_BACK);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FOLDER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        String path = "";
        if (resultCode == RESULT_OK && data != null && data.getData() != null) path = treeToPath(data.getData());
        picked = path;
    }

    /** A folder chosen in the picker ("content://...tree/primary%3ALADump%2F1.0.0") as a file path
     *  (/storage/emulated/0/LADump/1.0.0); a memory card's volume id (1234-ABCD) is /storage/1234-ABCD. */
    private static String treeToPath(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            int colon = id.indexOf(':');
            if (colon < 0) return "";
            String volume = id.substring(0, colon), rel = id.substring(colon + 1);
            String root = volume.equals("primary") ? Environment.getExternalStorageDirectory().getPath()
                                                   : "/storage/" + volume;
            return rel.isEmpty() ? root : root + "/" + rel;
        } catch (Exception e) {
            Log.e(TAG, "folder " + tree + " has no path", e);
            return "";
        }
    }
}
