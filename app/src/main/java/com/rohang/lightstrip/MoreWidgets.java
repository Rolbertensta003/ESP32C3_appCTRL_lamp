package com.rohang.lightstrip;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.format.DateFormat;
import android.view.View;
import android.widget.RemoteViews;

import org.json.JSONObject;

import java.util.Date;
import java.util.List;

import static com.rohang.lightstrip.LampWidgets.*;

/**
 * The widgets added in v0.3: Battery, Brightness, Colours, Modes, Timer and All lamps.
 * They share the lamp list, cached state and actions in LampWidgets.
 */
final class MoreWidgets {
    private MoreWidgets() {}

    /** Same presets as the app's colour swatches; the last one is Rainbow. */
    private static final String[] SWATCH_HEX = {"FF0000", "FF6400", "FFA014", "00DC3C", "00C8FF", "0050FF", "AA00FF", "FF288C", "rainbow"};
    private static final String[] SWATCH_NAME = {"Red", "Orange", "Amber", "Green", "Cyan", "Blue", "Purple", "Pink", "Rainbow"};
    private static final int[] SWATCH_IDS = {R.id.c_0, R.id.c_1, R.id.c_2, R.id.c_3, R.id.c_4, R.id.c_5, R.id.c_6, R.id.c_7, R.id.c_8};

    private static final String[] MODE_KEYS = {"color", "warm", "cold", "white", "rainbow", "blink", "music"};
    private static final int[] MODE_IDS = {R.id.md_0, R.id.md_1, R.id.md_2, R.id.md_3, R.id.md_4, R.id.md_5, R.id.md_6};
    private static final int[] MODE_DOTS = {R.id.md_d0, R.id.md_d1, R.id.md_d2, R.id.md_d3, R.id.md_d4, R.id.md_d5, R.id.md_d6};

    private static final int[] SLIDER_IDS = {R.id.r_s1, R.id.r_s2, R.id.r_s3, R.id.r_s4, R.id.r_s5,
            R.id.r_s6, R.id.r_s7, R.id.r_s8, R.id.r_s9, R.id.r_s10};

    private static final int[] TIMER_MIN = {15, 30, 60, 120};
    private static final int[] TIMER_IDS = {R.id.tm_15, R.id.tm_30, R.id.tm_60, R.id.tm_120};

    static void renderAll(Context c, AppWidgetManager m) {
        for (int id : ids(c, m, LampBatteryWidget.class)) m.updateAppWidget(id, battery(c, id));
        for (int id : ids(c, m, LampBrightnessWidget.class)) m.updateAppWidget(id, brightness(c, id));
        for (int id : ids(c, m, LampColourWidget.class)) m.updateAppWidget(id, colours(c, id));
        for (int id : ids(c, m, LampModesWidget.class)) m.updateAppWidget(id, modes(c, id));
        for (int id : ids(c, m, LampTimerWidget.class)) m.updateAppWidget(id, timer(c, id));
        for (int id : ids(c, m, LampAllWidget.class)) m.updateAppWidget(id, all(c, m, id));
    }

    private static int[] ids(Context c, AppWidgetManager m, Class<?> k) {
        return m.getAppWidgetIds(new ComponentName(c, k));
    }

    /* ---------------- Battery (2x1) ---------------- */

    private static RemoteViews battery(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_battery);
        Lamp l = lampFor(c, wid);
        v.setOnClickPendingIntent(R.id.b_icon_box, openApp(c));
        if (l == null) {
            v.setTextViewText(R.id.b_pct, "–");
            v.setTextViewText(R.id.b_sub, "Open the app to add a lamp");
            v.setImageViewResource(R.id.b_icon, R.drawable.widget_batt);
            v.setInt(R.id.b_icon, "setImageLevel", 0);
            v.setOnClickPendingIntent(R.id.b_root, openApp(c));
            return v;
        }
        JSONObject s = state(c, l.id);
        int st = status(c, l.id);
        int ink = c.getColor(R.color.w_ink);
        if (hasBattery(s)) {
            int p = batteryPct(s);
            boolean low = p <= BATT_LOW_PCT;
            v.setImageViewResource(R.id.b_icon, low ? R.drawable.widget_batt_low : R.drawable.widget_batt);
            v.setInt(R.id.b_icon, "setImageLevel", p * 100);
            v.setTextViewText(R.id.b_pct, p + "%");
            v.setTextColor(R.id.b_pct, low ? c.getColor(R.color.w_bad) : ink);
            String volts = String.format(java.util.Locale.US, "%.2f V", s.optDouble("bv", 0));
            v.setTextViewText(R.id.b_sub, st == ST_OFFLINE ? l.name + " · offline"
                    : l.name + " · " + (low ? "Low, charge soon" : volts));
            v.setContentDescription(R.id.b_root, l.name + " battery " + p + " percent. Tap to refresh.");
        } else {
            v.setImageViewResource(R.id.b_icon, R.drawable.widget_batt);
            v.setInt(R.id.b_icon, "setImageLevel", 0);
            v.setTextColor(R.id.b_pct, ink);
            v.setTextViewText(R.id.b_pct, st == ST_OFFLINE ? "Offline" : s == null ? "…" : "–");
            v.setTextViewText(R.id.b_sub, st == ST_OFFLINE ? l.name + " · tap to retry"
                    : s == null ? l.name + " · connecting" : l.name + " · no battery reported");
            v.setContentDescription(R.id.b_root, l.name + ", battery unknown. Tap to refresh.");
        }
        v.setOnClickPendingIntent(R.id.b_root, act(c, LampBatteryWidget.class, wid, OP_REFRESH, null));
        return v;
    }

    /* ---------------- Brightness (4x1) ---------------- */

    private static RemoteViews brightness(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_brightness);
        Class<?> k = LampBrightnessWidget.class;
        Lamp l = lampFor(c, wid);
        if (l == null) {
            v.setTextViewText(R.id.r_pct, "Add a lamp in the app");
            v.setInt(R.id.r_bar, "setImageLevel", 0);
            int[] all = {R.id.r_root, R.id.r_power, R.id.r_down, R.id.r_up};
            for (int id : all) v.setOnClickPendingIntent(id, openApp(c));
            for (int id : SLIDER_IDS) v.setOnClickPendingIntent(id, openApp(c));
            return v;
        }
        JSONObject s = state(c, l.id);
        int st = status(c, l.id);
        boolean on = isOn(s);
        int pct = s == null ? 0 : percent(s);
        v.setInt(R.id.r_power, "setBackgroundResource", on ? R.drawable.widget_power_on : R.drawable.widget_power);
        v.setImageViewResource(R.id.r_power, on ? R.drawable.ic_w_power_on : R.drawable.ic_w_power);
        v.setContentDescription(R.id.r_power, (on ? "Turn off " : "Turn on ") + l.name);
        v.setInt(R.id.r_bar, "setImageLevel", on ? pct * 100 : 0);
        String label = st == ST_OFFLINE ? "Offline · tap ↻" : s == null ? "…" : on ? pct + "%" : "Off · " + pct + "%";
        v.setTextViewText(R.id.r_pct, l.name + "  " + label);
        v.setOnClickPendingIntent(R.id.r_power, act(c, k, wid, OP_POWER, null));
        v.setOnClickPendingIntent(R.id.r_down, act(c, k, wid, OP_BRI_DOWN, null));
        v.setOnClickPendingIntent(R.id.r_up, act(c, k, wid, OP_BRI_UP, null));
        v.setOnClickPendingIntent(R.id.r_root, act(c, k, wid, OP_REFRESH, null));
        // Tap anywhere on the bar: the ten segments set 10% ... 100%.
        for (int i = 0; i < SLIDER_IDS.length; i++) {
            int p = (i + 1) * 10;
            v.setOnClickPendingIntent(SLIDER_IDS[i], act(c, k, wid, OP_BRI_SET, String.valueOf(p)));
            v.setContentDescription(SLIDER_IDS[i], "Brightness " + p + " percent");
        }
        return v;
    }

    /* ---------------- Colours (4x1) ---------------- */

    private static RemoteViews colours(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_colours);
        Lamp l = lampFor(c, wid);
        JSONObject s = l == null ? null : state(c, l.id);
        boolean on = isOn(s);
        String mode = s == null ? "" : s.optString("mode");
        String cur = s == null ? "" : String.format("%02X%02X%02X",
                clamp(s.optInt("r")), clamp(s.optInt("g")), clamp(s.optInt("b")));
        for (int i = 0; i < SWATCH_IDS.length; i++) {
            int id = SWATCH_IDS[i];
            boolean rainbow = "rainbow".equals(SWATCH_HEX[i]);
            if (rainbow) {
                v.setImageViewResource(id, R.drawable.widget_glow_rainbow);
                v.setInt(id, "setColorFilter", 0);
            } else {
                v.setImageViewResource(id, R.drawable.widget_glow);
                v.setInt(id, "setColorFilter", 0xFF000000 | Integer.parseInt(SWATCH_HEX[i], 16));
            }
            boolean picked = on && (rainbow ? "rainbow".equals(mode)
                    : ("color".equals(mode) || "blink".equals(mode)) && SWATCH_HEX[i].equals(cur));
            v.setInt(id, "setBackgroundResource", picked ? R.drawable.widget_swatch_on : R.drawable.widget_glow_ring);
            v.setContentDescription(id, SWATCH_NAME[i] + (picked ? ", selected" : ""));
            v.setOnClickPendingIntent(id, l == null ? openApp(c)
                    : act(c, LampColourWidget.class, wid, OP_COLOR, SWATCH_HEX[i]));
        }
        return v;
    }

    /* ---------------- Modes (4x1) ---------------- */

    private static RemoteViews modes(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_modes);
        Lamp l = lampFor(c, wid);
        JSONObject s = l == null ? null : state(c, l.id);
        boolean on = isOn(s);
        String mode = s == null ? "" : s.optString("mode");
        int rgb = s == null ? Color.WHITE
                : Color.rgb(clamp(s.optInt("r", 255)), clamp(s.optInt("g", 255)), clamp(s.optInt("b", 255)));
        for (int i = 0; i < MODE_KEYS.length; i++) {
            String key = MODE_KEYS[i];
            boolean active = on && key.equals(mode);
            v.setInt(MODE_IDS[i], "setBackgroundResource", active ? R.drawable.widget_chip_on : R.drawable.widget_chip);
            if ("rainbow".equals(key)) {
                v.setImageViewResource(MODE_DOTS[i], R.drawable.widget_glow_rainbow);
                v.setInt(MODE_DOTS[i], "setColorFilter", 0);
            } else {
                int col;
                switch (key) {
                    case "warm":  col = kelvin(s == null ? 2700 : s.optInt("warm", 2700)); break;
                    case "cold":  col = kelvin(s == null ? 6500 : s.optInt("cold", 6500)); break;
                    case "white": col = Color.WHITE; break;
                    default:      col = rgb;
                }
                v.setImageViewResource(MODE_DOTS[i], R.drawable.widget_glow);
                v.setInt(MODE_DOTS[i], "setColorFilter", col);
            }
            v.setContentDescription(MODE_IDS[i], modeTitle(key) + (active ? ", on" : ""));
            v.setOnClickPendingIntent(MODE_IDS[i], l == null ? openApp(c)
                    : act(c, LampModesWidget.class, wid, OP_MODE, key));
        }
        return v;
    }

    /* ---------------- Timer (2x2) ---------------- */

    private static RemoteViews timer(Context c, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_timer);
        Class<?> k = LampTimerWidget.class;
        Lamp l = lampFor(c, wid);
        if (l == null) {
            v.setTextViewText(R.id.tm_name, "Open the app to add a lamp");
            v.setViewVisibility(R.id.tm_count, View.GONE);
            v.setViewVisibility(R.id.tm_idle, View.VISIBLE);
            v.setTextViewText(R.id.tm_idle, "–");
            v.setTextViewText(R.id.tm_when, "");
            paintBattery(v, R.id.tm_batt_icon, R.id.tm_batt, null);
            v.setOnClickPendingIntent(R.id.tm_root, openApp(c));
            for (int id : TIMER_IDS) v.setOnClickPendingIntent(id, openApp(c));
            return v;
        }
        JSONObject s = state(c, l.id);
        int st = status(c, l.id);
        boolean on = isOn(s);
        paintBattery(v, R.id.tm_batt_icon, R.id.tm_batt, s);
        v.setTextViewText(R.id.tm_name, st == ST_OFFLINE ? l.name + " · not responding" : l.name);

        long now = System.currentTimeMillis();
        long at = wp(c).getLong("t_" + l.id, now);
        long offLeft = s == null ? 0 : s.optInt("offT", 0) * 1000L - (now - at);
        long onLeft = s == null ? 0 : s.optInt("onT", 0) * 1000L - (now - at);
        // The off delay only matters while the light is on; the on delay while it is off.
        String type = offLeft > 0 && (on || onLeft <= 0) ? "off" : onLeft > 0 ? "on" : null;
        long left = "off".equals(type) ? offLeft : onLeft;

        if (type != null) {
            v.setViewVisibility(R.id.tm_count, View.VISIBLE);
            v.setViewVisibility(R.id.tm_idle, View.GONE);
            v.setChronometer(R.id.tm_count, SystemClock.elapsedRealtime() + left, null, true);
            v.setChronometerCountDown(R.id.tm_count, true);
            String clock = DateFormat.getTimeFormat(c).format(new Date(now + left));
            v.setTextViewText(R.id.tm_title, "off".equals(type) ? "Sleep timer" : "On delay");
            v.setTextViewText(R.id.tm_when, ("off".equals(type) ? "Turns off at " : "Turns on at ") + clock);
            v.setViewVisibility(R.id.tm_chips, View.GONE);
            v.setViewVisibility(R.id.tm_cancel, View.VISIBLE);
            v.setOnClickPendingIntent(R.id.tm_cancel, act(c, k, wid, OP_TIMER, type + ":0"));
            scheduleRefresh(c, k, wid, now + left + 2000);
        } else {
            v.setChronometer(R.id.tm_count, SystemClock.elapsedRealtime(), null, false);
            v.setViewVisibility(R.id.tm_count, View.GONE);
            v.setViewVisibility(R.id.tm_idle, View.VISIBLE);
            v.setTextViewText(R.id.tm_title, "Sleep timer");
            v.setTextViewText(R.id.tm_idle, st == ST_BUSY ? "Sending…" : on ? "Not set" : "Light is off");
            v.setTextViewText(R.id.tm_when, on ? "Turn off after" : "Turn the light on to use");
            v.setViewVisibility(R.id.tm_chips, View.VISIBLE);
            v.setViewVisibility(R.id.tm_cancel, View.GONE);
        }
        for (int i = 0; i < TIMER_IDS.length; i++)
            v.setOnClickPendingIntent(TIMER_IDS[i], act(c, k, wid, OP_TIMER, "off:" + TIMER_MIN[i]));
        v.setOnClickPendingIntent(R.id.tm_root, act(c, k, wid, OP_REFRESH, null));
        return v;
    }

    /** Re-reads the lamp just after a timer ends so the widget doesn't keep counting below zero. */
    private static void scheduleRefresh(Context c, Class<?> k, int wid, long atMs) {
        AlarmManager am = (AlarmManager) c.getSystemService(Context.ALARM_SERVICE);
        if (am == null) return;
        PendingIntent pi = act(c, k, wid, OP_REFRESH, null);
        try { am.set(AlarmManager.RTC, atMs, pi); } catch (Exception ignored) {}
    }

    /* ---------------- All lamps (4x2) ---------------- */

    private static RemoteViews all(Context c, AppWidgetManager m, int wid) {
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.widget_all);
        Class<?> k = LampAllWidget.class;
        List<Lamp> lamps = lamps(c);
        v.removeAllViews(R.id.a_list);
        v.setOnClickPendingIntent(R.id.a_title, openApp(c));
        if (lamps.isEmpty()) {
            v.setViewVisibility(R.id.a_empty, View.VISIBLE);
            v.setViewVisibility(R.id.a_alloff, View.GONE);
            v.setOnClickPendingIntent(R.id.a_empty, openApp(c));
            v.setTextViewText(R.id.a_title, "All lamps");
            return v;
        }
        v.setViewVisibility(R.id.a_empty, View.GONE);
        int onCount = 0;
        int fit = rowsThatFit(m, wid);
        int rows = lamps.size() <= fit ? lamps.size() : Math.max(1, fit - 1);   // leave room for "+N more"
        for (Lamp l : lamps) if (isOn(state(c, l.id))) onCount++;
        v.setTextViewText(R.id.a_title, onCount == 0 ? "All lamps · off" : "All lamps · " + onCount + " on");
        v.setViewVisibility(R.id.a_alloff, onCount > 0 ? View.VISIBLE : View.GONE);
        v.setOnClickPendingIntent(R.id.a_alloff, act(c, k, wid, OP_ALL_OFF, null));

        for (int i = 0; i < rows; i++) {
            Lamp l = lamps.get(i);
            JSONObject s = state(c, l.id);
            int st = status(c, l.id);
            boolean on = isOn(s);
            RemoteViews r = new RemoteViews(c.getPackageName(), R.layout.widget_all_row);
            r.setTextViewText(R.id.row_name, l.name);
            String sub = st == ST_OFFLINE ? "Not responding" : st == ST_BUSY ? "Sending…" : s == null ? "Connecting…"
                    : on ? modeTitle(s.optString("mode")) + " · " + percent(s) + "%" : "Off";
            r.setTextViewText(R.id.row_status, sub);
            r.setTextColor(R.id.row_status, c.getColor(st == ST_OFFLINE ? R.color.w_bad : R.color.w_mute));
            paintGlow(r, R.id.row_glow, s);
            paintBattery(r, R.id.row_batt_icon, R.id.row_batt, s);
            r.setInt(R.id.row_power, "setBackgroundResource", on ? R.drawable.widget_power_on : R.drawable.widget_power);
            r.setImageViewResource(R.id.row_power, on ? R.drawable.ic_w_power_on : R.drawable.ic_w_power);
            r.setContentDescription(R.id.row_power, (on ? "Turn off " : "Turn on ") + l.name);
            r.setOnClickPendingIntent(R.id.row_power, act(c, k, wid, OP_LAMP_POWER, l.id));
            r.setOnClickPendingIntent(R.id.row_root, openLamp(c, l.id));
            v.addView(R.id.a_list, r);
        }
        if (lamps.size() > rows) {
            RemoteViews more = new RemoteViews(c.getPackageName(), R.layout.widget_all_more);
            more.setTextViewText(R.id.more_text, "+" + (lamps.size() - rows) + " more · make the widget taller");
            more.setOnClickPendingIntent(R.id.more_text, openApp(c));
            v.addView(R.id.a_list, more);
        }
        return v;
    }

    /** Rows of 44dp under a 40dp header; the widget reports its size in dp. */
    private static int rowsThatFit(AppWidgetManager m, int wid) {
        Bundle o = m.getAppWidgetOptions(wid);
        int h = o == null ? 0 : o.getInt(AppWidgetManager.OPTION_APPWIDGET_MAX_HEIGHT, 0);
        if (h <= 0) return 3;
        return Math.max(1, Math.min(8, (h - 24 - 40) / 44));
    }
}
