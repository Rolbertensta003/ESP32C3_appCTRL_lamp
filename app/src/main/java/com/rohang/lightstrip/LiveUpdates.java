package com.rohang.lightstrip;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.PowerManager;

import org.json.JSONObject;

/**
 * Background settings shared by the app, the live update service and the widgets:
 * whether live updates run, how often, low battery alerts, and the permission checks.
 */
final class LiveUpdates {
    private LiveUpdates() {}

    static final String CH_LIVE = "live", CH_BATTERY = "battery";
    static final int[] INTERVALS = {15, 30, 60, 300};
    private static final int RESET_ABOVE = 25;   // alert again only after the battery recovers past this

    private static SharedPreferences p(Context c) {
        return c.getSharedPreferences("lamp_live", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) { return p(c).getBoolean("live", false); }
    static int intervalSec(Context c) { return p(c).getInt("every", 30); }
    static boolean alerts(Context c) { return p(c).getBoolean("alerts", true); }
    static boolean asked(Context c) { return p(c).getBoolean("asked", false); }

    static void setEnabled(Context c, boolean on) {
        p(c).edit().putBoolean("live", on).apply();
        if (on) LiveUpdateService.start(c);
        else LiveUpdateService.stop(c);
    }

    static void setInterval(Context c, int sec) {
        int best = INTERVALS[1];
        for (int v : INTERVALS) if (v == sec) best = v;
        p(c).edit().putInt("every", best).apply();
        if (enabled(c)) LiveUpdateService.start(c);   // picks up the new interval
    }

    static void setAlerts(Context c, boolean on) { p(c).edit().putBoolean("alerts", on).apply(); }
    static void markAsked(Context c) { p(c).edit().putBoolean("asked", true).apply(); }

    /** True when Android lets the app run in the background without battery optimisation. */
    static boolean unrestricted(Context c) {
        PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
        return pm != null && pm.isIgnoringBatteryOptimizations(c.getPackageName());
    }

    static boolean notificationsAllowed(Context c) {
        if (Build.VERSION.SDK_INT >= 33
                && c.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            return false;
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        return nm != null && nm.areNotificationsEnabled();
    }

    static void channels(Context c) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm == null) return;
        NotificationChannel live = new NotificationChannel(CH_LIVE, "Live widget updates", NotificationManager.IMPORTANCE_MIN);
        live.setDescription("Shown while Light strip keeps your widgets up to date.");
        live.setShowBadge(false);
        NotificationChannel batt = new NotificationChannel(CH_BATTERY, "Low battery", NotificationManager.IMPORTANCE_DEFAULT);
        batt.setDescription("When a lamp's battery runs low.");
        nm.createNotificationChannel(live);
        nm.createNotificationChannel(batt);
    }

    /**
     * Posts one notification per lamp when its battery drops to the low level,
     * and again only after it has been charged above RESET_ABOVE.
     */
    static void checkBattery(Context c) {
        SharedPreferences pr = p(c);
        SharedPreferences.Editor e = pr.edit();
        boolean on = alerts(c);
        for (LampWidgets.Lamp l : LampWidgets.lamps(c)) {
            JSONObject s = LampWidgets.state(c, l.id);
            if (!LampWidgets.hasBattery(s)) continue;
            int pct = LampWidgets.batteryPct(s);
            String key = "warned_" + l.id;
            boolean warned = pr.getBoolean(key, false);
            if (pct <= LampWidgets.BATT_LOW_PCT && !warned) {
                // Only counts once the notification is really shown, so turning on alerts or
                // allowing notifications later still warns about a lamp that is already low.
                if (on && notifyLow(c, l, pct)) e.putBoolean(key, true);
            } else if (pct >= RESET_ABOVE && warned) {
                e.putBoolean(key, false);
                NotificationManager nm = c.getSystemService(NotificationManager.class);
                if (nm != null) nm.cancel(alertId(l));
            }
        }
        e.apply();
    }

    private static int alertId(LampWidgets.Lamp l) { return 1000 + (l.id.hashCode() & 0xFFFF); }

    private static boolean notifyLow(Context c, LampWidgets.Lamp l, int pct) {
        if (!notificationsAllowed(c)) return false;
        channels(c);
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm == null) return false;
        Notification n = builder(c, CH_BATTERY)
                .setSmallIcon(R.drawable.ic_stat_lamp)
                .setContentTitle(l.name + " battery is low")
                .setContentText(pct + "% left. Charge the lamp soon.")
                .setContentIntent(LampWidgets.openLamp(c, l.id))
                .setAutoCancel(true)
                .build();
        nm.notify(alertId(l), n);
        return true;
    }

    @SuppressWarnings("deprecation")
    static Notification.Builder builder(Context c, String channel) {
        return Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, channel) : new Notification.Builder(c);
    }

    static PendingIntent openAppIntent(Context c) {
        return LampWidgets.openApp(c);
    }

    static Intent appSettings(Context c) {
        return new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.parse("package:" + c.getPackageName()));
    }
}
