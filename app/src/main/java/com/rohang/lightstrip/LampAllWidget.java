package com.rohang.lightstrip;

import android.appwidget.AppWidgetManager;
import android.content.Context;
import android.os.Bundle;

/** 4x2 widget: every saved lamp with its power, mode and battery, plus All off. Shows more rows when resized taller. */
public class LampAllWidget extends BaseLampWidget {

    @Override
    public void onAppWidgetOptionsChanged(Context c, AppWidgetManager m, int id, Bundle options) {
        LampWidgets.renderAll(c);
    }
}
