package com.rohang.lightstrip;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;
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

        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient());
        web.setOverScrollMode(WebView.OVER_SCROLL_NEVER);
        web.addJavascriptInterface(new Bridge(), "Native");

        if (savedInstanceState != null) web.restoreState(savedInstanceState);
        else web.loadUrl("file:///android_asset/index.html");
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
    protected void onResume() { super.onResume(); js("window.appResume&&appResume()"); }

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

        private SharedPreferences prefs() {
            return getSharedPreferences("lamp", Context.MODE_PRIVATE);
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
