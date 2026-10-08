package com.rohang.lightstrip;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.view.View;
import android.widget.RemoteViews;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Shared logic for the home-screen widgets: which lamp a widget controls,
 * talking to the lamp, the cached lamp state, and drawing both widget sizes.
 * Lamps are read from the same "lamp" preferences the app saves through Native.save("lamps", ...).
 */
final class LampWidgets {
    private LampWidgets() {}

    static final String ACTION = "com.rohang.lightstrip.WIDGET_ACTION";
    static final String OP_POWER = "power", OP_BRI_UP = "bri_up", OP_BRI_DOWN = "bri_down",
            OP_MODE = "mode", OP_SLEEP = "sleep", OP_REFRESH = "refresh";
    /** v0.3 widgets: arg = percent / hex colour / "off:30" / lamp id. */
    static final String OP_BRI_SET = "bri_set", OP_COLOR = "color", OP_TIMER = "timer",
            OP_LAMP_POWER = "lamp_power", OP_ALL_OFF = "all_off";
    /** Firmware v0.3 shows "Low" on the OLED at or below this. */
    static final int BATT_LOW_PCT = 20;

    static final int ST_UNKNOWN = 0, ST_OK = 1, ST_OFFLINE = 2, ST_BUSY = 3;
    private static final int SLEEP_MINUTES = 30;
    static final int OFF_GREY = 0xFF8A8D93;
    private static final String[] MODE_KEYS = {"warm", "cold", "color", "rainbow"};

    /** One thread, so quick repeated taps reach the lamp in order. */
    private static final ExecutorService IO = Executors.newSingleThreadExecutor();

    static final class Lamp {
        final String id, name, addr;
        Lamp(String id, String name, String addr) { this.id = id; this.name = name; this.addr = addr; }
    }

    /* ---------------- lamps and per-widget choice ---------------- */

    private static JSONObject appData(Context c) {
        try {
            return new JSONObject(c.getSharedPreferences("lamp", Context.MODE_PRIVATE).getString("lamps", "{}"));
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    static List<Lamp> lamps(Context c) {
        List<Lamp> out = new ArrayList<>();
        JSONArray a = appData(c).optJSONArray("list");
        if (a == null) return out;
        for (int i = 0; i < a.length(); i++) {
            JSONObject o = a.optJSONObject(i);
            if (o == null || o.optString("addr").isEmpty()) continue;
            out.add(new Lamp(o.optString("id"), o.optString("name", "Lamp"), o.optString("addr")));
        }
        return out;
    }

    static SharedPreferences wp(Context c) {
        return c.getSharedPreferences("lamp_widget", Context.MODE_PRIVATE);
    }

    /** The lamp picked for this widget; falls back to the app's current lamp, then the first one. */
    static Lamp lampFor(Context c, int widgetId) {
        List<Lamp> all = lamps(c);
        if (all.isEmpty()) return null;
        String want = wp(c).getString("w_" + widgetId, "");
        String active = appData(c).optString("active", "");
        Lamp fallback = null;
        for (Lamp l : all) {
            if (l.id.equals(want)) return l;
            if (l.id.equals(active)) fallback = l;
        }
        return fallback != null ? fallback : all.get(0);
    }

    static Lamp lampById(Context c, String lampId) {
        for (Lamp l : lamps(c)) if (l.id.equals(lampId)) return l;
        return null;
    }

    static void assign(Context c, int widgetId, String lampId) {
        wp(c).edit().putString("w_" + widgetId, lampId).apply();
    }

    static void forget(Context c, int[] widgetIds) {
        SharedPreferences.Editor e = wp(c).edit();
        for (int id : widgetIds) e.remove("w_" + id);
        e.apply();
    }

    /* ---------------- cached state ---------------- */

    static JSONObject state(Context c, String lampId) {
        String s = wp(c).getString("s_" + lampId, "");
        if (s.isEmpty()) return null;
        try { return new JSONObject(s); } catch (Exception e) { return null; }
    }

    static int status(Context c, String lampId) {
        return wp(c).getInt("st_" + lampId, ST_UNKNOWN);
    }

    static void setStatus(Context c, String lampId, int st) {
        wp(c).edit().putInt("st_" + lampId, st).apply();
    }

    static void saveState(Context c, String lampId, JSONObject s, int st, boolean fromLamp) {
        SharedPreferences.Editor e = wp(c).edit().putString("s_" + lampId, s.toString()).putInt("st_" + lampId, st);
        if (fromLamp) e.putLong("t_" + lampId, System.currentTimeMillis());
        e.apply();
    }

    /** Minutes left on the off timer, counted from when the lamp last reported it. */
    static int sleepMinutesLeft(Context c, String lampId, JSONObject s) {
        if (s == null) return 0;
        int offT = s.optInt("offT", 0);
        if (offT <= 0) return 0;
        long at = wp(c).getLong("t_" + lampId, 0);
        long left = offT - (System.currentTimeMillis() - at) / 1000;
        return left <= 0 ? 0 : (int) ((left + 59) / 60);
    }

    static boolean isOn(JSONObject s) {
        return s != null && !"off".equals(s.optString("mode", "off"));
    }

    static int percent(JSONObject s) {
        return Math.round(s.optInt("bri", 128) * 100f / 255f);
    }

    /* ---------------- battery (firmware v0.3: bok, bv, bp) ---------------- */

    /** False for older firmware or when no battery is wired to the lamp. */
    static boolean hasBattery(JSONObject s) {
        return s != null && s.optBoolean("bok", false);
    }

    static int batteryPct(JSONObject s) {
        return Math.max(0, Math.min(100, s.optInt("bp", 0)));
    }

    static boolean batteryLow(JSONObject s) {
        return hasBattery(s) && batteryPct(s) <= BATT_LOW_PCT;
    }

    /** Battery icon (filled to the level) plus "82%". Both hidden when the lamp reports no battery. */
    static void paintBattery(RemoteViews v, int iconId, int textId, JSONObject s) {
        boolean has = hasBattery(s);
        v.setViewVisibility(iconId, has ? View.VISIBLE : View.GONE);
        v.setViewVisibility(textId, has ? View.VISIBLE : View.GONE);
        if (!has) return;
        int p = batteryPct(s);
        boolean low = p <= BATT_LOW_PCT;
        v.setImageViewResource(iconId, low ? R.drawable.widget_batt_low : R.drawable.widget_batt);
        v.setInt(iconId, "setImageLevel", p * 100);
        v.setTextViewText(textId, p + "%");
        v.setContentDescription(textId, "Battery " + p + " percent" + (low ? ", low" : ""));
    }

    /** Small "82%" badge for the 1x1 switch; red when low. */
    static void paintBadge(Context c, RemoteViews v, int textId, JSONObject s) {
        boolean has = hasBattery(s);
        v.setViewVisibility(textId, has ? View.VISIBLE : View.GONE);
        if (!has) return;
        int p = batteryPct(s);
        v.setTextViewText(textId, p + "%");
        v.setTextColor(textId, c.getColor(p <= BATT_LOW_PCT ? R.color.w_bad : R.color.w_ink));
        v.setContentDescription(textId, "Battery " + p + " percent");
    }

    /** Next brightness in 10% steps, never below 5%. Returns the 0-255 value the lamp expects. */
    private static int stepBri(JSONObject s, boolean up) {
        int p = s == null ? 50 : percent(s);
        int n = up ? (p / 10) * 10 + 10 : ((p + 9) / 10) * 10 - 10;
        n = Math.max(5, Math.min(100, n));
        return Math.max(1, Math.round(n * 255f / 100f));
    }

    /* ---------------- actions ---------------- */

    static void perform(Context app, int widgetId, String op, String arg, BroadcastReceiver.PendingResult pr) {
        IO.execute(() -> {
            try {
                if (OP_ALL_OFF.equals(op)) allOffNow(app);
                else performNow(app, widgetId, op, arg);
                LiveUpdates.checkBattery(app);
            }
            finally { if (pr != null) pr.finish(); }
        });
    }

    /** All lamps widget: turns off every lamp that is on (or not known yet). */
    private static void allOffNow(Context c) {
        List<Lamp> all = lamps(c);
        for (Lamp l : all) {
            JSONObject s = state(c, l.id);
            if (s != null && !isOn(s)) continue;
            try {
                if (s != null) saveState(c, l.id, guess(OP_POWER, null, s), ST_BUSY, false);
            } catch (Exception ignored) {}
        }
        renderAll(c);
        for (Lamp l : all) {
            JSONObject s = state(c, l.id);
            if (s != null && status(c, l.id) != ST_BUSY && !isOn(s)) continue;
            try {
                JSONObject now = fetch(l.addr, "/api/set?power=0");
                if (now == null || !now.has("mode")) throw new IOException("not a lamp");
                saveState(c, l.id, now, ST_OK, true);
            } catch (Exception e) {
                setStatus(c, l.id, ST_OFFLINE);
            }
        }
        renderAll(c);
    }

    private static void performNow(Context c, int widgetId, String op, String arg) {
        Lamp l = OP_LAMP_POWER.equals(op) ? lampById(c, arg) : lampFor(c, widgetId);
        if (l == null) { renderAll(c); return; }
        try {
            JSONObject s = state(c, l.id);
            if (s == null && !OP_REFRESH.equals(op)) s = fetch(l.addr, "/api/state");
            String path = pathFor(op, arg, s, sleepMinutesLeft(c, l.id, s));

            // Show the change straight away, then confirm with what the lamp reports.
            JSONObject guess = guess(op, arg, s);
            if (guess != null) saveState(c, l.id, guess, ST_BUSY, false);
            else setStatus(c, l.id, ST_BUSY);
            renderAll(c);

            JSONObject now = fetch(l.addr, path);
            if (now == null || !now.has("mode")) now = fetch(l.addr, "/api/state");
            if (now == null || !now.has("mode")) throw new IOException("not a lamp");
            saveState(c, l.id, now, ST_OK, true);
        } catch (Exception e) {
            setStatus(c, l.id, ST_OFFLINE);
        }
        renderAll(c);
    }

    private static String pathFor(String op, String arg, JSONObject s, int sleepLeft) {
        switch (op) {
            case OP_POWER:    return "/api/set?power=" + (isOn(s) ? "0" : "1");
            case OP_BRI_UP:   return "/api/set?bri=" + stepBri(s, true);
            case OP_BRI_DOWN: return "/api/set?bri=" + stepBri(s, false);
            case OP_MODE:     return "/api/set?mode=" + Uri.encode(arg == null ? "warm" : arg);
            case OP_SLEEP:    return "/api/timer?type=off&min=" + (sleepLeft > 0 ? 0 : SLEEP_MINUTES);
            case OP_LAMP_POWER: return "/api/set?power=" + (isOn(s) ? "0" : "1");
            case OP_BRI_SET:  return "/api/set?bri=" + briFromPercent(arg);
            case OP_COLOR:    return "rainbow".equals(arg) ? "/api/set?mode=rainbow"
                                    : "/api/set?mode=" + colourMode(s) + "&c=" + Uri.encode(arg == null ? "FFFFFF" : arg);
            case OP_TIMER:    return timerPath(arg);
            default:          return "/api/state";
        }
    }

    private static JSONObject guess(String op, String arg, JSONObject s) {
        if (s == null) return null;
        try {
            JSONObject g = new JSONObject(s.toString());
            switch (op) {
                case OP_POWER:
                case OP_LAMP_POWER:
                    if (isOn(s)) { g.put("last", s.optString("mode")); g.put("mode", "off"); }
                    else g.put("mode", s.optString("last", "warm"));
                    return g;
                case OP_BRI_SET:
                    g.put("bri", briFromPercent(arg));
                    return g;
                case OP_COLOR:
                    if ("rainbow".equals(arg)) { g.put("mode", "rainbow"); g.put("last", "rainbow"); return g; }
                    int rgb = Integer.parseInt(arg, 16);
                    String m = colourMode(s);
                    g.put("mode", m);
                    g.put("last", m);
                    g.put("r", rgb >> 16 & 255);
                    g.put("g", rgb >> 8 & 255);
                    g.put("b", rgb & 255);
                    return g;
                case OP_BRI_UP:
                case OP_BRI_DOWN:
                    g.put("bri", stepBri(s, OP_BRI_UP.equals(op)));
                    return g;
                case OP_MODE:
                    g.put("mode", arg);
                    g.put("last", arg);
                    return g;
                default:
                    return null;
            }
        } catch (Exception e) {
            return null;
        }
    }

    /** Same 0-255 conversion as the brightness buttons; never fully dark. */
    private static int briFromPercent(String arg) {
        int p;
        try { p = Integer.parseInt(arg); } catch (Exception e) { p = 50; }
        p = Math.max(5, Math.min(100, p));
        return Math.max(1, Math.round(p * 255f / 100f));
    }

    /** Like the app: picking a colour while blinking keeps Blink, otherwise switches to Colour. */
    private static String colourMode(JSONObject s) {
        return s != null && "blink".equals(s.optString("mode")) ? "blink" : "color";
    }

    /** arg is "off:30" or "on:15"; 0 minutes cancels that timer. */
    private static String timerPath(String arg) {
        String type = "off", min = "0";
        if (arg != null) {
            int k = arg.indexOf(':');
            if (k > 0) { type = arg.substring(0, k); min = arg.substring(k + 1); }
        }
        if (!"on".equals(type)) type = "off";
        return "/api/timer?type=" + type + "&min=" + Uri.encode(min);
    }

    /** Re-reads every lamp that has a widget. delayMs lets the app's last command land first. */
    static void refreshAll(Context ctx, BroadcastReceiver.PendingResult pr, long delayMs) {
        refreshAll(ctx, pr, delayMs, false, null);
    }

    /**
     * withAllLamps also reads lamps that have no widget (live updates use it for low battery alerts).
     * done runs on the widget thread once every lamp has answered or timed out.
     */
    static void refreshAll(Context ctx, BroadcastReceiver.PendingResult pr, long delayMs,
                           boolean withAllLamps, Runnable done) {
        final Context c = ctx.getApplicationContext();
        IO.execute(() -> {
            try {
                if (delayMs > 0) Thread.sleep(delayMs);
                Set<String> seen = new HashSet<>();
                List<Lamp> todo = new ArrayList<>();
                for (int id : allIds(c)) {
                    Lamp l = lampFor(c, id);
                    if (l != null && seen.add(l.id)) todo.add(l);
                }
                if (withAllLamps || hasWidgets(c, LampAllWidget.class))
                    for (Lamp l : lamps(c)) if (seen.add(l.id)) todo.add(l);
                for (Lamp l : todo) {
                    try {
                        JSONObject s = fetch(l.addr, "/api/state");
                        if (s == null || !s.has("mode")) throw new IOException("not a lamp");
                        saveState(c, l.id, s, ST_OK, true);
                    } catch (Exception e) {
                        setStatus(c, l.id, ST_OFFLINE);
                    }
                }
                renderAll(c);
                LiveUpdates.checkBattery(c);
            } catch (InterruptedException ignored) {
            } finally {
                if (pr != null) pr.finish();
                if (done != null) done.run();   // always, so live updates keep their schedule
            }
        });
    }

    /** The app shares each state it reads, so widgets follow the app while it is open. */
    static void fromApp(Context ctx, String lampId, String json) {
        final Context c = ctx.getApplicationContext();
        IO.execute(() -> {
            try {
                JSONObject s = new JSONObject(json);
                if (!s.has("mode") || lampById(c, lampId) == null) return;
                saveState(c, lampId, s, ST_OK, true);
                renderAll(c);
                LiveUpdates.checkBattery(c);
            } catch (Exception ignored) {}
        });
    }

    static JSONObject fetch(String addr, String path) throws IOException {
        HttpURLConnection h = (HttpURLConnection) new URL("http://" + addr + path).openConnection();
        h.setConnectTimeout(2500);
        h.setReadTimeout(3000);
        h.setUseCaches(false);
        try {
            int code = h.getResponseCode();
            if (code >= 400) throw new IOException("HTTP " + code);
            try (InputStream in = h.getInputStream(); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buf = new byte[2048];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                try { return new JSONObject(out.toString("UTF-8")); } catch (Exception e) { return null; }
            }
        } finally {
            h.disconnect();
        }
    }

    /* ---------------- drawing ---------------- */

    /** Every widget that controls one lamp (the All lamps widget is handled separately). */
    static int[] allIds(Context c) {
        AppWidgetManager m = AppWidgetManager.getInstance(c);
        Class<?>[] kinds = {LampWidgetProvider.class, LampToggleWidget.class, LampBatteryWidget.class,
                LampBrightnessWidget.class, LampColourWidget.class, LampModesWidget.class, LampTimerWidget.class};
        List<Integer> ids = new ArrayList<>();
        for (Class<?> k : kinds)
            for (int id : m.getAppWidgetIds(new ComponentName(c, k))) ids.add(id);
        int[] all = new int[ids.size()];
        for (int i = 0; i < all.length; i++) all[i] = ids.get(i);
        return all;
    }

    static boolean hasWidgets(Context c, Class<?> k) {
        return AppWidgetManager.getInstance(c).getAppWidgetIds(new ComponentName(c, k)).length > 0;
    }

    static void renderAll(Context c) {
        AppWidgetManager m = AppWidgetManager.getInstance(c);
        for (int id : m.getAppWidgetIds(new ComponentName(c, LampWidgetProvider.class)))
            m.updateAppWidget(id, buildControls(c, id));
        for (int id : m.getAppWidgetIds(new ComponentName(c, LampToggleWidget.class)))
            m.updateAppWidget(id, buildToggle(c, id));
        MoreWidgets.renderAll(c, m);
    }

    private static RemoteViews buildControls(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_lamp);
        Class<?> k = LampWidgetProvider.class;
        int[] modeIds = {R.id.w_m_warm, R.id.w_m_cold, R.id.w_m_color, R.id.w_m_rainbow};
        PendingIntent open = openApp(c);
        v.setOnClickPendingIntent(R.id.w_glow, open);
        v.setOnClickPendingIntent(R.id.w_name, open);

        Lamp l = lampFor(c, wid);
        if (l == null) {
            v.setTextViewText(R.id.w_name, "No lamp yet");
            showStatus(v, "Open the app to add one", false);
            paintGlow(v, R.id.w_glow, null);
            paintBattery(v, R.id.w_batt_icon, R.id.w_batt, null);
            v.setTextViewText(R.id.w_bri, "–");
            v.setInt(R.id.w_power, "setBackgroundResource", R.drawable.widget_power);
            v.setImageViewResource(R.id.w_power, R.drawable.ic_w_power);
            v.setTextViewText(R.id.w_sleep, "Sleep");
            v.setInt(R.id.w_sleep, "setBackgroundResource", R.drawable.widget_chip);
            for (int id : modeIds) v.setInt(id, "setBackgroundResource", R.drawable.widget_chip);
            int[] rest = {R.id.w_status, R.id.w_status_bad, R.id.w_power, R.id.w_bri_up, R.id.w_bri_down, R.id.w_sleep};
            for (int id : rest) v.setOnClickPendingIntent(id, open);
            for (int id : modeIds) v.setOnClickPendingIntent(id, open);
            return v;
        }

        JSONObject s = state(c, l.id);
        int st = status(c, l.id);
        boolean on = isOn(s);
        String mode = s == null ? "" : s.optString("mode", "");
        int sleep = sleepMinutesLeft(c, l.id, s);

        v.setTextViewText(R.id.w_name, l.name);
        if (st == ST_BUSY) showStatus(v, "Sending…", false);
        else if (st == ST_OFFLINE) showStatus(v, "Not responding · tap to retry", true);
        else if (s == null) showStatus(v, "Connecting…", false);
        else if (!on) showStatus(v, "Off", false);
        else showStatus(v, modeTitle(mode) + (sleep > 0 ? " · off in " + sleep + " min" : ""), false);

        paintGlow(v, R.id.w_glow, s);
        paintBattery(v, R.id.w_batt_icon, R.id.w_batt, s);
        v.setInt(R.id.w_power, "setBackgroundResource", on ? R.drawable.widget_power_on : R.drawable.widget_power);
        v.setImageViewResource(R.id.w_power, on ? R.drawable.ic_w_power_on : R.drawable.ic_w_power);
        v.setContentDescription(R.id.w_power, (on ? "Turn off " : "Turn on ") + l.name);
        v.setTextViewText(R.id.w_bri, s == null ? "–" : percent(s) + "%");

        for (int i = 0; i < modeIds.length; i++) {
            boolean active = on && MODE_KEYS[i].equals(mode);
            v.setInt(modeIds[i], "setBackgroundResource", active ? R.drawable.widget_chip_on : R.drawable.widget_chip);
            v.setOnClickPendingIntent(modeIds[i], act(c, k, wid, OP_MODE, MODE_KEYS[i]));
        }
        v.setTextViewText(R.id.w_sleep, sleep > 0 ? sleep + "m left" : "Sleep");
        v.setInt(R.id.w_sleep, "setBackgroundResource", sleep > 0 ? R.drawable.widget_chip_on : R.drawable.widget_chip);
        v.setContentDescription(R.id.w_sleep, sleep > 0 ? "Cancel sleep timer" : "Turn off in " + SLEEP_MINUTES + " minutes");

        v.setOnClickPendingIntent(R.id.w_power, act(c, k, wid, OP_POWER, null));
        v.setOnClickPendingIntent(R.id.w_bri_up, act(c, k, wid, OP_BRI_UP, null));
        v.setOnClickPendingIntent(R.id.w_bri_down, act(c, k, wid, OP_BRI_DOWN, null));
        v.setOnClickPendingIntent(R.id.w_sleep, act(c, k, wid, OP_SLEEP, null));
        v.setOnClickPendingIntent(R.id.w_status, act(c, k, wid, OP_REFRESH, null));
        v.setOnClickPendingIntent(R.id.w_status_bad, act(c, k, wid, OP_REFRESH, null));
        return v;
    }

    private static RemoteViews buildToggle(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_toggle);
        Lamp l = lampFor(c, wid);
        if (l == null) {
            v.setTextViewText(R.id.t_name, "Add lamp");
            paintGlow(v, R.id.t_glow, null);
            v.setViewVisibility(R.id.t_batt, View.GONE);
            v.setInt(R.id.t_icon, "setColorFilter", Color.WHITE);
            v.setOnClickPendingIntent(R.id.t_root, openApp(c));
            return v;
        }
        JSONObject s = state(c, l.id);
        int st = status(c, l.id);
        boolean on = isOn(s);
        v.setTextViewText(R.id.t_name, st == ST_OFFLINE ? "Offline" : l.name);
        paintGlow(v, R.id.t_glow, s);
        paintBadge(c, v, R.id.t_batt, s);
        boolean rainbow = on && "rainbow".equals(s.optString("mode"));
        int icon = on && !rainbow && luminance(glowColor(s)) > 0.6 ? 0xFF1C1D20 : Color.WHITE;
        v.setInt(R.id.t_icon, "setColorFilter", icon);
        v.setContentDescription(R.id.t_root, (on ? "Turn off " : "Turn on ") + l.name);
        v.setOnClickPendingIntent(R.id.t_root, act(c, LampToggleWidget.class, wid, OP_POWER, null));
        return v;
    }

    private static void showStatus(RemoteViews v, String text, boolean bad) {
        v.setTextViewText(bad ? R.id.w_status_bad : R.id.w_status, text);
        v.setViewVisibility(R.id.w_status, bad ? View.GONE : View.VISIBLE);
        v.setViewVisibility(R.id.w_status_bad, bad ? View.VISIBLE : View.GONE);
    }

    static void paintGlow(RemoteViews v, int id, JSONObject s) {
        if (isOn(s) && "rainbow".equals(s.optString("mode"))) {
            v.setImageViewResource(id, R.drawable.widget_glow_rainbow);
            v.setInt(id, "setColorFilter", 0);   // transparent filter leaves the gradient as drawn
            return;
        }
        v.setImageViewResource(id, R.drawable.widget_glow);
        v.setInt(id, "setColorFilter", glowColor(s));
    }

    static int glowColor(JSONObject s) {
        if (!isOn(s)) return OFF_GREY;
        switch (s.optString("mode")) {
            case "warm":  return kelvin(s.optInt("warm", 2700));
            case "cold":  return kelvin(s.optInt("cold", 6500));
            case "white": return Color.WHITE;
            default:      return Color.rgb(clamp(s.optInt("r", 255)), clamp(s.optInt("g", 255)), clamp(s.optInt("b", 255)));
        }
    }

    /** Same colour-temperature curve the app uses. */
    static int kelvin(int k) {
        double t = k / 100.0, r, g, b;
        if (t <= 66) {
            r = 255;
            g = 99.4708025861 * Math.log(t) - 161.1195681661;
            b = t <= 19 ? 0 : 138.5177312231 * Math.log(t - 10) - 305.0447927307;
        } else {
            r = 329.698727446 * Math.pow(t - 60, -0.1332047592);
            g = 288.1221695283 * Math.pow(t - 60, -0.0755148492);
            b = 255;
        }
        return Color.rgb(clamp((int) Math.round(r)), clamp((int) Math.round(g)), clamp((int) Math.round(b)));
    }

    static int clamp(int x) { return Math.max(0, Math.min(255, x)); }

    static double luminance(int c) {
        return (0.299 * Color.red(c) + 0.587 * Color.green(c) + 0.114 * Color.blue(c)) / 255.0;
    }

    static String modeTitle(String m) {
        switch (m) {
            case "color":   return "Colour";
            case "warm":    return "Warm white";
            case "cold":    return "Cold white";
            case "white":   return "White";
            case "rainbow": return "Rainbow";
            case "blink":   return "Blink";
            case "music":   return "Music";
            default:        return "On";
        }
    }

    static int piFlags() {
        return PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
    }

    static PendingIntent openApp(Context c) {
        Intent i = new Intent(c, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        return PendingIntent.getActivity(c, 1, i, piFlags());
    }

    /** Opens the app with this lamp selected (All lamps widget rows). */
    static PendingIntent openLamp(Context c, String lampId) {
        Intent i = new Intent(c, MainActivity.class)
                .setData(Uri.parse("lampopen://" + Uri.encode(lampId)))
                .putExtra(MainActivity.EXTRA_LAMP, lampId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(c, 2, i, piFlags());
    }

    static PendingIntent act(Context c, Class<?> k, int wid, String op, String arg) {
        Intent i = new Intent(c, k)
                .setAction(ACTION)
                .setData(Uri.parse("lampwidget://" + wid + "/" + op + "/" + (arg == null ? "" : arg)))
                .putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, wid)
                .putExtra("op", op)
                .putExtra("arg", arg);
        return PendingIntent.getBroadcast(c, 0, i, piFlags());
    }
}
