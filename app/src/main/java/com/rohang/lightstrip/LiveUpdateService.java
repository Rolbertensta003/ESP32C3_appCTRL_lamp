package com.rohang.lightstrip;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import org.json.JSONObject;

import java.util.List;

/**
 * Keeps the widgets current while the screen is on: re-reads the lamps every few seconds
 * (15 s to 5 min, set in the app) and once straight away when the screen turns on.
 * Sleeps while the screen is off, so it costs almost no battery.
 * Runs as a foreground service with a silent notification, which Android requires for this.
 */
public class LiveUpdateService extends Service {

    private static final int NOTE_ID = 1;
    private static final String ACTION_STOP = "com.rohang.lightstrip.LIVE_STOP";

    private final Handler main = new Handler(Looper.getMainLooper());
    private boolean screenOn = true, running = false;

    static void start(Context c) {
        Intent i = new Intent(c, LiveUpdateService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Exception ignored) {
            // Android 12+ refuses to start it from the background unless the app is
            // exempt from battery optimisation. It starts next time the app opens.
        }
    }

    static void stop(Context c) {
        c.stopService(new Intent(c, LiveUpdateService.class));
    }

    private final BroadcastReceiver screen = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            screenOn = Intent.ACTION_SCREEN_ON.equals(i.getAction());
            main.removeCallbacks(tick);
            if (screenOn) main.post(tick);   // fresh widgets the moment the phone wakes up
        }
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!screenOn || running) return;
            running = true;
            boolean all = LiveUpdates.alerts(LiveUpdateService.this);
            LampWidgets.refreshAll(LiveUpdateService.this, null, 0, all, () -> main.post(() -> {
                running = false;
                updateNote();
                main.removeCallbacks(tick);
                if (screenOn) main.postDelayed(tick, LiveUpdates.intervalSec(LiveUpdateService.this) * 1000L);
            }));
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        LiveUpdates.channels(this);
        goForeground();
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        screenOn = pm == null || pm.isInteractive();
        IntentFilter f = new IntentFilter(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_SCREEN_OFF);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(screen, f, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(screen, f);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            LiveUpdates.setEnabled(this, false);
            stopSelf();
            return START_NOT_STICKY;
        }
        goForeground();
        if (!LiveUpdates.enabled(this)) { stopSelf(); return START_NOT_STICKY; }
        main.removeCallbacks(tick);
        main.post(tick);
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        main.removeCallbacks(tick);
        try { unregisterReceiver(screen); } catch (Exception ignored) {}
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private void goForeground() {
        Notification n = note();
        if (Build.VERSION.SDK_INT >= 34) startForeground(NOTE_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else startForeground(NOTE_ID, n);
    }

    private void updateNote() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.notify(NOTE_ID, note());
    }

    /** "Bedroom: on, 82% battery" for one lamp, "3 lamps, 2 on" for several. */
    private Notification note() {
        List<LampWidgets.Lamp> lamps = LampWidgets.lamps(this);
        String text;
        if (lamps.isEmpty()) text = "Add a lamp in the app";
        else if (lamps.size() == 1) {
            LampWidgets.Lamp l = lamps.get(0);
            JSONObject s = LampWidgets.state(this, l.id);
            int st = LampWidgets.status(this, l.id);
            text = l.name + ": " + (st == LampWidgets.ST_OFFLINE ? "not responding" : s == null ? "connecting"
                    : LampWidgets.isOn(s) ? "on" : "off")
                    + (LampWidgets.hasBattery(s) ? ", battery " + LampWidgets.batteryPct(s) + "%" : "");
        } else {
            int on = 0;
            for (LampWidgets.Lamp l : lamps) if (LampWidgets.isOn(LampWidgets.state(this, l.id))) on++;
            text = lamps.size() + " lamps, " + (on == 0 ? "all off" : on + " on");
        }
        Intent stop = new Intent(this, LiveUpdateService.class).setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(this, 3, stop, LampWidgets.piFlags());
        Notification.Builder b = LiveUpdates.builder(this, LiveUpdates.CH_LIVE)
                .setSmallIcon(R.drawable.ic_stat_lamp)
                .setContentTitle("Widgets are live")
                .setContentText(text)
                .setContentIntent(LiveUpdates.openAppIntent(this))
                .setOngoing(true)
                .setShowWhen(false)
                .addAction(new Notification.Action.Builder(null, "Stop", stopPi).build());
        if (Build.VERSION.SDK_INT < 26) b.setPriority(Notification.PRIORITY_MIN);
        return b.build();
    }
}
