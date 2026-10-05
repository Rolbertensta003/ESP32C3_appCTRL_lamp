package com.rohang.lightstrip;

import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.content.Intent;

/** Common receiver for both widget sizes. Button taps arrive here as LampWidgets.ACTION broadcasts. */
public abstract class BaseLampWidget extends AppWidgetProvider {

    @Override
    public void onReceive(Context c, Intent i) {
        if (LampWidgets.ACTION.equals(i.getAction())) {
            int wid = i.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
            String op = i.getStringExtra("op");
            if (wid != AppWidgetManager.INVALID_APPWIDGET_ID && op != null)
                LampWidgets.perform(c.getApplicationContext(), wid, op, i.getStringExtra("arg"), goAsync());
            return;
        }
        super.onReceive(c, i);
    }

    @Override
    public void onUpdate(Context c, AppWidgetManager m, int[] ids) {
        LampWidgets.renderAll(c);
        LampWidgets.refreshAll(c, goAsync(), 0);
    }

    @Override
    public void onDeleted(Context c, int[] ids) {
        LampWidgets.forget(c, ids);
    }
}
