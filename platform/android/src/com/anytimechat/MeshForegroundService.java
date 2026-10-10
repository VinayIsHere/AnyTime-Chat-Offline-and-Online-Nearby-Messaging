package com.anytimechat;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Keeps the app process alive so the C++ mesh survives the user leaving the app.
 *
 * <p>That is its <em>entire</em> job. The C++ core — services, protocol, transports —
 * lives in this same process, so holding a foreground notification is enough to keep
 * all of it running. There is no mesh logic here and there must never be: see the
 * dumb-pipe rule in docs/ARCHITECTURE.md section 11.
 *
 * <p>Without this, Android kills the process shortly after the activity stops. Announces
 * would cease, inbound chat requests would never arrive, and the app would appear to
 * work right up until the user switched away from it.
 *
 * <p><b>Started by BleGattTransport once the radio is up</b>, and stopped with it. Not
 * earlier: Android 14 refuses a {@code connectedDevice} foreground service until the
 * Bluetooth permissions are granted, and a permanent notification for a radio that is
 * not running would be misleading.
 */
public final class MeshForegroundService extends Service {

    private static final String TAG = "AnyTimeMesh";
    private static final String CHANNEL_ID = "anytime.mesh";
    private static final int NOTIFICATION_ID = 1;

    /** Starts the service. Safe to call more than once; never throws. */
    public static void start(Context context) {
        Context app = context.getApplicationContext();
        Intent intent = new Intent(app, MeshForegroundService.class);
        try {
            app.startForegroundService(intent);  // API 26; minSdk is 28
        } catch (IllegalStateException | SecurityException e) {
            // IllegalStateException covers ForegroundServiceStartNotAllowedException:
            // Android 12+ refuses to start one while the app is in the background. The
            // mesh keeps running for as long as the process does; it just is not
            // protected from being reclaimed.
            Log.w(TAG, "could not start the mesh foreground service", e);
        }
    }

    public static void stop(Context context) {
        Context app = context.getApplicationContext();
        app.stopService(new Intent(app, MeshForegroundService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // The type must match the manifest declaration; Android 14 rejects the
                // call otherwise.
                startForeground(NOTIFICATION_ID, buildNotification(),
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTIFICATION_ID, buildNotification());
            }
        } catch (RuntimeException e) {
            // SecurityException when a required permission is missing, or
            // ForegroundServiceStartNotAllowedException. Either way, stop rather than
            // let the system crash the whole app for a missed startForeground().
            Log.e(TAG, "mesh foreground service refused", e);
            stopSelf();
            return START_NOT_STICKY;
        }

        Log.i(TAG, "mesh foreground service running");

        // NOT_STICKY: if Android kills the process, restarting this service alone
        // would bring back the notification without the C++ mesh it stands for —
        // that only starts with the app itself.
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;  // not a bound service
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "mesh foreground service stopped");
        super.onDestroy();
    }

    private void createChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "Nearby messaging",
                // LOW: no sound, no heads-up. This notification exists because the
                // OS requires one, not because the user needs to be told anything.
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps AnyTime reachable to people nearby.");
        channel.setShowBadge(false);

        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        // Tapping the notification returns to the app rather than doing nothing.
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent pending = null;
        if (launch != null) {
            launch.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            pending = PendingIntent.getActivity(
                    this, 0, launch,
                    PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        }

        // minSdk is 28, so the channel-aware builder (API 26) is always available.
        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("AnyTime is on")
                .setContentText("Discoverable to people nearby")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setOngoing(true)
                .setContentIntent(pending)
                .build();
    }
}
