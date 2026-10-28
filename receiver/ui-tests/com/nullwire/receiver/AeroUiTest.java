package com.nullwire.receiver;

import android.os.Bundle;
import android.view.View;
import android.widget.*;
import androidx.appcompat.app.AppCompatActivity;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28)
public class AeroUiTest {
    public static class UiOnlyActivity extends AppCompatActivity {
        AeroAppearance appearance;
        protected void onCreate(Bundle saved) {
            setTheme(R.style.Theme_NullWireNative);
            super.onCreate(saved);
            setContentView(R.layout.activity_main);
            appearance = new AeroAppearance(this);
            appearance.bind(0);
        }
    }
    @Test public void themesAndNavigationKeepTheSameControls() {
        try (var controller = Robolectric.buildActivity(UiOnlyActivity.class).setup()) {
            var activity = controller.get();
            var meter = activity.findViewById(R.id.pbLevelMeter);
            ((ProgressBar)meter).setProgress(55);
            int[] tabs = {R.id.navStream,R.id.navSound,R.id.navDevices,R.id.navSettings};
            int[] pages = {R.id.pageStream,R.id.pageSound,R.id.pageDevices,R.id.pageSettings};
            for (int theme : new int[]{R.id.themeDark,R.id.themeLight}) {
                activity.findViewById(theme).performClick();
                assertSame(meter, activity.findViewById(R.id.pbLevelMeter));
                assertEquals(55, ((ProgressBar)meter).getProgress());
                assertTrue(activity.findViewById(theme).isSelected());
                for (int i=0;i<tabs.length;i++) {
                    activity.findViewById(tabs[i]).performClick();
                    for (int j=0;j<pages.length;j++) assertEquals(i==j ? View.VISIBLE : View.GONE, activity.findViewById(pages[j]).getVisibility());
                }
            }
            assertFalse(activity.isFinishing());
        }
    }
    @Test public void presetsUseExistingSeekbarListeners() {
        try (var controller = Robolectric.buildActivity(UiOnlyActivity.class).setup()) {
            var activity = controller.get();
            SeekBar bass = activity.findViewById(R.id.sbBass);
            SeekBar treble = activity.findViewById(R.id.sbTreble);
            int[] changed = {0};
            bass.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                public void onProgressChanged(SeekBar s,int value,boolean fromUser) { changed[0]++; }
                public void onStartTrackingTouch(SeekBar s) {}
                public void onStopTrackingTouch(SeekBar s) {}
            });
            activity.findViewById(R.id.presetCinema).performClick();
            assertEquals(4,bass.getProgress()); assertEquals(3,treble.getProgress());
            assertEquals(1,changed[0]);
            activity.findViewById(R.id.themeDark).performClick();
            assertEquals(4,bass.getProgress()); assertEquals(1,changed[0]);
        }
    }
}
