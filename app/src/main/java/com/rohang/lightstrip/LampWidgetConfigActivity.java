package com.rohang.lightstrip;

import android.app.Activity;
import android.app.AlertDialog;
import android.appwidget.AppWidgetManager;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;

import java.util.List;

/** Shown when a widget is placed (or reconfigured): pick which lamp it controls. */
public class LampWidgetConfigActivity extends Activity {

    private int wid = AppWidgetManager.INVALID_APPWIDGET_ID;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Bundle x = getIntent().getExtras();
        if (x != null) wid = x.getInt(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
        setResult(RESULT_CANCELED, result());
        if (wid == AppWidgetManager.INVALID_APPWIDGET_ID) { finish(); return; }

        final List<LampWidgets.Lamp> lamps = LampWidgets.lamps(this);
        if (lamps.size() == 1) { choose(lamps.get(0)); return; }

        AlertDialog.Builder d = new AlertDialog.Builder(this, dialogTheme());
        if (lamps.isEmpty()) {
            d.setTitle("No lamps yet")
             .setMessage("Open Light strip and add your lamp first, then add the widget again.")
             .setPositiveButton("Open app", (di, w) -> {
                 startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                 finish();
             })
             .setNegativeButton("Cancel", (di, w) -> finish());
        } else {
            String[] names = new String[lamps.size()];
            for (int i = 0; i < names.length; i++) names[i] = lamps.get(i).name + "  (" + lamps.get(i).addr + ")";
            d.setTitle("Which lamp should this widget control?")
             .setItems(names, (di, w) -> choose(lamps.get(w)))
             .setNegativeButton("Cancel", (di, w) -> finish());
        }
        d.setOnCancelListener(di -> finish());
        d.show();
    }

    private void choose(LampWidgets.Lamp l) {
        LampWidgets.assign(this, wid, l.id);
        LampWidgets.renderAll(this);
        LampWidgets.refreshAll(this, null, 0);
        setResult(RESULT_OK, result());
        finish();
    }

    private Intent result() {
        return new Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, wid);
    }

    private int dialogTheme() {
        int night = getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return night == Configuration.UI_MODE_NIGHT_YES
                ? android.R.style.Theme_DeviceDefault_Dialog_Alert
                : android.R.style.Theme_DeviceDefault_Light_Dialog_Alert;
    }
}
