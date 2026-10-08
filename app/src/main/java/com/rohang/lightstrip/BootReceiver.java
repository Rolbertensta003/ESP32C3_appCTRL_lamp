package com.rohang.lightstrip;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Restarts live widget updates after the phone restarts or the app is updated. */
public class BootReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context c, Intent i) {
        String a = i.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) return;
        if (LiveUpdates.enabled(c)) LiveUpdateService.start(c);
        LampWidgets.renderAll(c);
        LampWidgets.refreshAll(c, goAsync(), 0);
    }
}
