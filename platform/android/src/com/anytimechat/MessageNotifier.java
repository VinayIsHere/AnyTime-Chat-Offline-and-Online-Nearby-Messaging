package com.anytimechat;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Posts "new message" and "new chat request" notifications for someone who has left the app.
 *
 * <p>Called over JNI by AndroidNotificationPresenter. <b>What</b> to say — whose name, which
 * text, where a tap goes — was already decided in C++ (adapters/notify/Notifier.cpp), where it
 * is unit tested. This class only decides <b>whether</b> to show it, from state only Android
 * holds, and then shows it. No protocol logic here, as everywhere in this package.
 *
 * <p>Not shown when:
 * <ul>
 *   <li>the app is on screen — the page already shows the message live, and a heads-up on top
 *       of the conversation it belongs to is noise;
 *   <li>the user declined POST_NOTIFICATIONS (API 33+). Declining is allowed: chatting nearby
 *       must still work, so this is a quiet no-op, never a prompt.
 * </ul>
 *
 * <p><b>Lock screen:</b> the message text is private. The public version says only that
 * something arrived, because a chat app built around not being overheard should not read
 * messages aloud to whoever picks up the phone.
 */
public final class MessageNotifier {

    private static final String TAG = "AnyTimeNotify";

    /** Mirrors notify::NoticeKind — keep in step with AndroidNotificationPresenter.cpp. */
    public static final int KIND_MESSAGE = 0;
    public static final int KIND_REQUEST = 1;

    private static final String CHANNEL_MESSAGES = "anytime.messages";
    private static final String CHANNEL_REQUESTS = "anytime.requests";

    /** Notification ids per kind; the thread is the tag, so each chat is its own entry. */
    private static final int ID_MESSAGE = 100;
    private static final int ID_REQUEST = 101;

    /** How many recent lines one chat's notification shows. */
    private static final int MAX_LINES = 5;

    /** Read by AnyTimeActivity when it is launched from a notification. */
    public static final String EXTRA_ROUTE = "com.anytimechat.ROUTE";

    /**
     * Recent lines per thread, so a second message expands the notification instead of
     * replacing the first. In memory only: it exists to render the notification, and it is
     * cleared the moment the user opens the app.
     */
    private static final Map<String, List<String>> LINES = new LinkedHashMap<>();

    private MessageNotifier() {}

    public static void show(Context context, int kind, String thread, String title, String text,
                            String route) {
        if (AnyTimeActivity.isInForeground()) return;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED) {
            return;
        }

        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null) return;
        ensureChannels(manager);

        final boolean isMessage = kind == KIND_MESSAGE;
        List<String> lines;
        synchronized (LINES) {
            lines = LINES.get(thread);
            if (lines == null) {
                lines = new ArrayList<>();
                LINES.put(thread, lines);
            }
            lines.add(text);
            while (lines.size() > MAX_LINES) lines.remove(0);
            lines = new ArrayList<>(lines);
        }

        Notification.InboxStyle style = new Notification.InboxStyle();
        for (String line : lines) style.addLine(line);

        Notification publicVersion =
                new Notification.Builder(context, isMessage ? CHANNEL_MESSAGES : CHANNEL_REQUESTS)
                        .setSmallIcon(android.R.drawable.stat_notify_chat)
                        .setContentTitle("AnyTime")
                        .setContentText(isMessage ? "New message" : "New chat request")
                        .build();

        Notification notification =
                new Notification.Builder(context, isMessage ? CHANNEL_MESSAGES : CHANNEL_REQUESTS)
                        .setSmallIcon(android.R.drawable.stat_notify_chat)
                        .setContentTitle(title)
                        .setContentText(text)
                        .setStyle(lines.size() > 1 ? style : new Notification.BigTextStyle().bigText(text))
                        .setNumber(lines.size())
                        .setCategory(isMessage ? Notification.CATEGORY_MESSAGE
                                               : Notification.CATEGORY_SOCIAL)
                        .setVisibility(Notification.VISIBILITY_PRIVATE)
                        .setPublicVersion(publicVersion)
                        .setAutoCancel(true)
                        .setContentIntent(openApp(context, thread, route))
                        .build();

        try {
            manager.notify(thread, isMessage ? ID_MESSAGE : ID_REQUEST, notification);
        } catch (RuntimeException e) {
            // A permission revoked between the check and the call, or an OEM quirk.
            // Missing one notification is not worth the process.
            Log.w(TAG, "could not post a notification", e);
        }
    }

    /**
     * Removes every notification this class posted. Called when the app comes to the
     * foreground: the user is now looking at the app itself, which shows everything the
     * notifications were pointing at.
     */
    public static void clearAll(Context context) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        synchronized (LINES) {
            if (manager != null) {
                for (String thread : LINES.keySet()) {
                    manager.cancel(thread, ID_MESSAGE);
                    manager.cancel(thread, ID_REQUEST);
                }
            }
            LINES.clear();
        }
    }

    private static PendingIntent openApp(Context context, String thread, String route) {
        Intent intent = new Intent(context, AnyTimeActivity.class);
        intent.putExtra(EXTRA_ROUTE, route);
        // singleTop in the manifest, so a running app receives this in onNewIntent
        // instead of a second copy being stacked on top of it.
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        // One request code per thread, or every chat's notification would share one
        // PendingIntent and all open whichever chat posted last.
        return PendingIntent.getActivity(context, thread.hashCode(), intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static void ensureChannels(NotificationManager manager) {
        if (manager.getNotificationChannel(CHANNEL_MESSAGES) == null) {
            NotificationChannel messages = new NotificationChannel(
                    CHANNEL_MESSAGES, "Messages", NotificationManager.IMPORTANCE_HIGH);
            messages.setDescription("Messages from your contacts nearby.");
            messages.setLockscreenVisibility(Notification.VISIBILITY_PRIVATE);
            manager.createNotificationChannel(messages);
        }
        if (manager.getNotificationChannel(CHANNEL_REQUESTS) == null) {
            NotificationChannel requests = new NotificationChannel(
                    CHANNEL_REQUESTS, "Chat requests", NotificationManager.IMPORTANCE_DEFAULT);
            requests.setDescription("Someone nearby wants to chat.");
            requests.setLockscreenVisibility(Notification.VISIBILITY_PRIVATE);
            manager.createNotificationChannel(requests);
        }
    }
}
