package com.rohang.lightstrip;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Light strip controller.
 * The interface lives in assets/index.html. Requests to the lamp go through the
 * native Bridge below, so the ESP32 does not need CORS headers and plain HTTP works.
 */
public class MainActivity extends Activity {

    private WebView web;
    private final ExecutorService pool = Executors.newFixedThreadPool(4);
    private final Handler main = new Handler(Looper.getMainLooper());
    private static final int PICK_FILE = 7;
    private ValueCallback<Uri[]> fileCb;

    static final String EXTRA_LAMP = "lamp";
    private static final int REQ_NOTIF = 8;
    private boolean pageReady = false, thenBattery = false;
    private String pendingLamp;   // lamp picked on the All lamps widget, applied once the page is ready

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        web = new WebView(this);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setMediaPlaybackRequiresUserGesture(true);
        s.setTextZoom(100);   // keep layout stable when system font size is large

        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView v, String url) {
                pageReady = true;
                openPendingLamp();
            }
        });
        pendingLamp = getIntent().getStringExtra(EXTRA_LAMP);
        web.setWebChromeClient(new WebChromeClient() {
            /** Lets <input type="file"> in the page open the system picker (used by Display picture). */
            @Override
            public boolean onShowFileChooser(WebView v, ValueCallback<Uri[]> cb, FileChooserParams p) {
                if (fileCb != null) fileCb.onReceiveValue(null);
                fileCb = cb;
                Intent i = new Intent(Intent.ACTION_GET_CONTENT).addCategory(Intent.CATEGORY_OPENABLE).setType("image/*");
                try { startActivityForResult(Intent.createChooser(i, "Choose a picture"), PICK_FILE); }
                catch (Exception e) { fileCb = null; cb.onReceiveValue(null); return false; }
                return true;
            }
        });
        web.setOverScrollMode(WebView.OVER_SCROLL_NEVER);
        web.addJavascriptInterface(new Bridge(), "Native");

        if (savedInstanceState != null) web.restoreState(savedInstanceState);
        else web.loadUrl("file:///android_asset/index.html");

        // The app is in the foreground here, so Android always allows starting live updates.
        if (LiveUpdates.enabled(this)) LiveUpdateService.start(this);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String id = intent.getStringExtra(EXTRA_LAMP);
        if (id != null) { pendingLamp = id; if (pageReady) openPendingLamp(); }
    }

    private void openPendingLamp() {
        if (pendingLamp == null) return;
        js("window.openLamp&&openLamp(" + JSONObject.quote(pendingLamp) + ")");
        pendingLamp = null;
    }

    /* ---------------- background permission (live widgets) ---------------- */

    /** Notifications first (Android 13+), then the battery optimisation exemption. */
    private void askNotifications(boolean battAfter) {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            thenBattery = battAfter;
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
            return;
        }
        if (!LiveUpdates.notificationsAllowed(this)) openNotificationSettings();
        else if (battAfter) askUnrestricted();
        bgChanged();
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        super.onRequestPermissionsResult(req, perms, res);
        if (req != REQ_NOTIF) return;
        if (LiveUpdates.enabled(this)) LiveUpdateService.start(this);   // refresh its notification
        if (thenBattery) askUnrestricted();
        thenBattery = false;
        bgChanged();
    }

    @SuppressLint("BatteryLife")
    private void askUnrestricted() {
        if (LiveUpdates.unrestricted(this)) { bgChanged(); return; }
        try {
            startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            try { startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)); }
            catch (Exception e2) { openAppSettings(); }
        }
    }

    private void openNotificationSettings() {
        try {
            if (Build.VERSION.SDK_INT >= 26)
                startActivity(new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                        .putExtra(Settings.EXTRA_APP_PACKAGE, getPackageName()));
            else openAppSettings();
        } catch (Exception e) { openAppSettings(); }
    }

    private void openAppSettings() {
        try { startActivity(LiveUpdates.appSettings(this)); } catch (Exception ignored) {}
    }

    private void bgChanged() { js("window.bgChanged&&bgChanged()"); }

    @SuppressWarnings("deprecation")
    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK_FILE || fileCb == null) return;
        Uri u = res == RESULT_OK && data != null ? data.getData() : null;
        fileCb.onReceiveValue(u == null ? null : new Uri[]{u});
        fileCb = null;
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        web.saveState(out);
    }

    @Override
    protected void onPause() {
        super.onPause();
        js("window.appPause&&appPause()");
        LampWidgets.refreshAll(this, null, 800);   // widgets pick up what was just changed in the app
    }

    @Override
    protected void onResume() { super.onResume(); js("window.appResume&&appResume()"); bgChanged(); }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        web.evaluateJavascript("window.appBack?appBack():false", value -> {
            if (!"true".equals(value)) MainActivity.super.onBackPressed();
        });
    }

    @Override
    protected void onDestroy() {
        pool.shutdownNow();
        web.destroy();
        super.onDestroy();
    }

    private void js(String code) { main.post(() -> { if (web != null) web.evaluateJavascript(code, null); }); }

    /** Methods callable from the page as window.Native.* */
    class Bridge {

        /** Async HTTP request. Result arrives in window.__native(id, status, body); status 0 = no response. */
        @JavascriptInterface
        public void request(final String id, final String method, final String url,
                            final String body, final int timeoutMs) {
            pool.execute(() -> {
                int status = 0;
                String text;
                HttpURLConnection c = null;
                try {
                    c = (HttpURLConnection) new URL(url).openConnection();
                    int t = timeoutMs > 0 ? timeoutMs : 5000;
                    c.setConnectTimeout(t);
                    c.setReadTimeout(t);
                    c.setUseCaches(false);
                    c.setRequestMethod(method);
                    c.setRequestProperty("Cache-Control", "no-store");
                    if ("POST".equals(method)) {
                        byte[] b = (body == null ? "" : body).getBytes(StandardCharsets.UTF_8);
                        c.setDoOutput(true);
                        c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                        c.setFixedLengthStreamingMode(b.length);
                        try (OutputStream os = c.getOutputStream()) { os.write(b); }
                    }
                    status = c.getResponseCode();
                    InputStream is = status >= 400 ? c.getErrorStream() : c.getInputStream();
                    text = is == null ? "" : readAll(is);
                } catch (Exception e) {
                    status = 0;
                    text = e.getClass().getSimpleName();
                } finally {
                    if (c != null) c.disconnect();
                }
                js("window.__native(" + JSONObject.quote(id) + "," + status + "," + JSONObject.quote(text) + ")");
            });
        }

        @JavascriptInterface
        public String load(String key) {
            return prefs().getString(key, "");
        }

        @JavascriptInterface
        public void save(String key, String value) {
            prefs().edit().putString(key, value).apply();
            if ("lamps".equals(key)) LampWidgets.renderAll(MainActivity.this);   // renames / new IPs show on widgets
        }

        @JavascriptInterface
        public void openUrl(String url) {
            main.post(() -> {
                try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
                catch (Exception e) { toast("No browser found"); }
            });
        }

        @JavascriptInterface
        public void toast(String msg) {
            main.post(() -> Toast.makeText(MainActivity.this, msg, Toast.LENGTH_SHORT).show());
        }

        /* ----- v0.3: widgets, background and battery ----- */

        /** Widgets copy the state the app just read, so they match the app while it is open. */
        @JavascriptInterface
        public void widgetState(String lampId, String json) {
            LampWidgets.fromApp(MainActivity.this, lampId, json);
        }

        /** Everything the Widgets and background card shows. */
        @JavascriptInterface
        public String bgStatus() {
            Context c = MainActivity.this;
            try {
                JSONObject o = new JSONObject()
                        .put("unrestricted", LiveUpdates.unrestricted(c))
                        .put("notif", LiveUpdates.notificationsAllowed(c))
                        .put("live", LiveUpdates.enabled(c))
                        .put("every", LiveUpdates.intervalSec(c))
                        .put("alerts", LiveUpdates.alerts(c))
                        .put("asked", LiveUpdates.asked(c))
                        .put("canPin", Build.VERSION.SDK_INT >= 26
                                && AppWidgetManager.getInstance(c).isRequestPinAppWidgetSupported());
                JSONObject counts = new JSONObject();
                for (String k : WIDGET_KINDS)
                    counts.put(k, AppWidgetManager.getInstance(c).getAppWidgetIds(new ComponentName(c, widgetClass(k))).length);
                return o.put("widgets", counts).toString();
            } catch (Exception e) {
                return "{}";
            }
        }

        /** First-run setup: live updates on, then ask for notifications and background use. */
        @JavascriptInterface
        public void setupBackground() {
            main.post(() -> {
                LiveUpdates.markAsked(MainActivity.this);
                LiveUpdates.setEnabled(MainActivity.this, true);
                askNotifications(true);
            });
        }

        @JavascriptInterface
        public void markAsked() { LiveUpdates.markAsked(MainActivity.this); }

        @JavascriptInterface
        public void allowBackground() { main.post(MainActivity.this::askUnrestricted); }

        @JavascriptInterface
        public void allowNotifications() { main.post(() -> askNotifications(false)); }

        @JavascriptInterface
        public void openAppSettings() { main.post(MainActivity.this::openAppSettings); }

        @JavascriptInterface
        public void setLive(boolean on) {
            main.post(() -> { LiveUpdates.setEnabled(MainActivity.this, on); bgChanged(); });
        }

        @JavascriptInterface
        public void setEvery(int sec) { LiveUpdates.setInterval(MainActivity.this, sec); }

        @JavascriptInterface
        public void setAlerts(boolean on) {
            LiveUpdates.setAlerts(MainActivity.this, on);
            if (on && !LiveUpdates.notificationsAllowed(MainActivity.this)) main.post(() -> askNotifications(false));
        }

        /** Asks the launcher to place a widget (Android 8+). Returns false when the launcher can't. */
        @JavascriptInterface
        public boolean addWidget(String kind) {
            if (Build.VERSION.SDK_INT < 26) return false;
            AppWidgetManager m = AppWidgetManager.getInstance(MainActivity.this);
            if (!m.isRequestPinAppWidgetSupported()) return false;
            try {
                return m.requestPinAppWidget(new ComponentName(MainActivity.this, widgetClass(kind)), null, null);
            } catch (Exception e) {
                return false;
            }
        }

        private SharedPreferences prefs() {
            return getSharedPreferences("lamp", Context.MODE_PRIVATE);
        }
    }

    private static final String[] WIDGET_KINDS = {"controls", "switch", "battery", "brightness", "colours", "modes", "timer", "all"};

    private static Class<?> widgetClass(String kind) {
        switch (kind == null ? "" : kind) {
            case "switch":     return LampToggleWidget.class;
            case "battery":    return LampBatteryWidget.class;
            case "brightness": return LampBrightnessWidget.class;
            case "colours":    return LampColourWidget.class;
            case "modes":      return LampModesWidget.class;
            case "timer":      return LampTimerWidget.class;
            case "all":        return LampAllWidget.class;
            default:           return LampWidgetProvider.class;
        }
    }

    private static String readAll(InputStream is) throws Exception {
        try (InputStream in = is; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }
}
