package com.anytimechat;

import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;

import androidx.core.content.FileProvider;

import org.anytimechat.app.R;
import org.qtproject.qt.android.bindings.QtActivity;

import java.io.File;
import java.io.IOException;
import java.util.regex.Pattern;

/**
 * QtActivity subclass that makes {@code <input type="file">} work, and that opens the right
 * screen when a notification is tapped.
 *
 * <p><b>Opening a chat from a notification</b> is done by NAVIGATING the WebView to
 * {@code <base>/chats/<id>} — the server already answers any app route with index.html, so
 * the React router takes it from there. That is a URL load, not a bridge: nothing is injected
 * into the page and nothing new can reach the core. The route is checked against a strict
 * pattern first, because this activity is exported as the launcher and any app could send it
 * an intent with that extra.
 *
 * <p>The file-chooser half, originally this class's only job:
 *
 * <p><b>Why this class has to exist.</b> Qt's WebView ships its own
 * {@code QtAndroidWebViewController$QtAndroidWebChromeClient}, and it overrides exactly four
 * methods: onProgressChanged, onReceivedIcon, onReceivedTitle and
 * onGeolocationPermissionsShowPrompt. It does <b>not</b> override
 * {@link WebChromeClient#onShowFileChooser}. Android's WebView calls that method to open a
 * picker; with no override the default returns false and <b>nothing happens at all</b> — no
 * intent, no dialog, no error. A tap on the attach button was silently inert, which a device
 * log confirms: not one picker intent across the whole session.
 *
 * <p>That cannot be fixed from TypeScript, so the chrome client is replaced here.
 *
 * <p><b>WHAT THIS DELIBERATELY DOES NOT DO.</b> It installs no JavaScript bridge. FunMeet,
 * which this pattern comes from, subclasses QtActivity for both reasons — the file chooser
 * AND {@code addJavascriptInterface} for its native API. AnyTime needs only the first: the
 * page authenticates by knowing its secret URL and reaches the core over ordinary loopback
 * HTTP (AD-03 / AD-08), so there is nothing to inject and no bridge to maintain. Keep it
 * that way — a JS bridge would be a second, weaker path into the core.
 *
 * <p><b>Cost of replacing Qt's chrome client:</b> Qt's four overrides above are lost. Nothing
 * in AnyTime consumes them — the QML shell watches {@code onLoadingChanged}, which comes from
 * the WebViewClient rather than the chrome client, and we use no title, icon, progress or
 * geolocation. If a future change needs one, re-implement it here rather than reverting.
 *
 * <p><b>Camera needs no CAMERA permission</b>, and it is deliberately not declared. The photo
 * is taken by the system camera app through ACTION_IMAGE_CAPTURE and handed back as a URI.
 * Declaring CAMERA without holding it would actually make that throw, so the absence is
 * load-bearing rather than an oversight.
 */
public class AnyTimeActivity extends QtActivity {

    private static final String TAG = "AnyTime";

    /** Distinct from PermissionBridge's 4711/4712. */
    private static final int FILE_CHOOSER_REQUEST = 4713;

    /** Qt builds its view tree asynchronously, so the WebView is not there at onCreate. */
    private static final int  WEBVIEW_MAX_RETRIES = 20;
    private static final long WEBVIEW_FIRST_DELAY_MS = 1500;
    private static final long WEBVIEW_RETRY_DELAY_MS = 500;

    private int webViewRetryCount = 0;

    /** Read by MessageNotifier from the Qt thread, hence volatile. */
    private static volatile boolean inForeground = false;

    /** The only routes a notification may open. Anything else is ignored. */
    private static final Pattern ROUTE = Pattern.compile("^(requests|chats/[A-Za-z0-9_-]{1,128})$");

    /** Page load can take a while on a cold start; give up after ~20 s. */
    private static final int  ROUTE_MAX_RETRIES = 40;
    private static final long ROUTE_RETRY_DELAY_MS = 500;

    private String pendingRoute;
    private int routeRetryCount = 0;

    public static boolean isInForeground() {
        return inForeground;
    }

    private ValueCallback<Uri[]> filePathCallback;
    private Uri cameraImageUri;

    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        new Handler(Looper.getMainLooper()).postDelayed(this::installFileChooser,
                                                        WEBVIEW_FIRST_DELAY_MS);
        takeRoute(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        takeRoute(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        inForeground = true;
        // The user is looking at the app, which shows everything the notifications
        // were pointing at.
        MessageNotifier.clearAll(this);
    }

    @Override
    protected void onPause() {
        inForeground = false;
        super.onPause();
    }

    /** Picks up a route a notification tap carried, once, and starts applying it. */
    private void takeRoute(Intent intent) {
        if (intent == null) return;
        String route = intent.getStringExtra(MessageNotifier.EXTRA_ROUTE);
        intent.removeExtra(MessageNotifier.EXTRA_ROUTE);  // a rotation must not replay it
        if (route == null) return;
        if (!ROUTE.matcher(route).matches()) {
            Log.w(TAG, "ignoring a launch route that is not an app screen");
            return;
        }
        pendingRoute = route;
        routeRetryCount = 0;
        applyRoute();
    }

    /**
     * Loads {@code <base><route>} once the app page itself has loaded.
     *
     * <p>Waiting for the page matters on a cold start: the QML shell loads the base URL on its
     * own, and loading ours before it finishes would race it. The base is taken from the
     * page's own URL — {@code http://127.0.0.1:<port>/<token>/...} — because the secret prefix
     * changes every launch and Java is never told it any other way.
     */
    private void applyRoute() {
        if (pendingRoute == null) return;

        WebView webView = findWebView(getWindow().getDecorView());
        String url = webView != null ? webView.getUrl() : null;
        Uri current = url != null ? Uri.parse(url) : null;
        boolean ready = current != null
                && "127.0.0.1".equals(current.getHost())
                && !current.getPathSegments().isEmpty()
                && webView.getProgress() == 100;

        if (!ready) {
            if (++routeRetryCount >= ROUTE_MAX_RETRIES) {
                Log.w(TAG, "page never loaded; opening the app without the notification's route");
                pendingRoute = null;
                return;
            }
            new Handler(Looper.getMainLooper()).postDelayed(this::applyRoute, ROUTE_RETRY_DELAY_MS);
            return;
        }

        String base = current.getScheme() + "://" + current.getEncodedAuthority() + "/"
                + current.getPathSegments().get(0) + "/";
        webView.loadUrl(base + pendingRoute);
        pendingRoute = null;
    }

    /**
     * Finds Qt's WebView and replaces its chrome client.
     *
     * <p>The retry loop is unavoidable: Qt creates the WebView some time after onCreate, and
     * there is no callback that says when. Giving up after a bounded number of attempts is
     * better than looping forever — the app still works, just without photo attachment.
     */
    private void installFileChooser() {
        WebView webView = findWebView(getWindow().getDecorView());

        if (webView == null) {
            if (++webViewRetryCount >= WEBVIEW_MAX_RETRIES) {
                Log.e(TAG, "WebView not found after " + WEBVIEW_MAX_RETRIES
                           + " attempts; photo attachment will not work");
                return;
            }
            new Handler(Looper.getMainLooper()).postDelayed(this::installFileChooser,
                                                            WEBVIEW_RETRY_DELAY_MS);
            return;
        }

        // The theme's window background is the launch splash (res/drawable/splash.xml),
        // which is right for the first frame and wrong ever after: anywhere the page does
        // not cover the window — a keyboard resizing it, say — the logo would show through.
        // The WebView existing means Qt's own views now cover the window, so this swap is
        // never seen.
        getWindow().setBackgroundDrawableResource(R.color.splash_background);

        WebSettings settings = webView.getSettings();
        // Needed so the WebView can read the content:// URI the picker returns.
        // setAllowFileAccess is deliberately NOT enabled: our page is served over
        // loopback HTTP and never needs file:// access, so leaving it off keeps one
        // more door shut on an origin that can read every chat.
        settings.setAllowContentAccess(true);

        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view,
                                             ValueCallback<Uri[]> callback,
                                             FileChooserParams params) {
                // A picker already open means the previous one never resolved. Release it,
                // or the WebView keeps waiting on a callback that will never fire and every
                // later tap is ignored.
                if (filePathCallback != null) filePathCallback.onReceiveValue(null);
                filePathCallback = callback;

                Intent chooser = buildChooser();
                try {
                    startActivityForResult(chooser, FILE_CHOOSER_REQUEST);
                } catch (RuntimeException e) {
                    Log.w(TAG, "no activity could handle the image chooser", e);
                    filePathCallback.onReceiveValue(null);
                    filePathCallback = null;
                    return false;
                }
                return true;
            }
        });

        Log.i(TAG, "file chooser installed on Qt's WebView");
    }

    /** Gallery plus camera, in one system chooser. */
    private Intent buildChooser() {
        Intent gallery = new Intent(Intent.ACTION_GET_CONTENT);
        gallery.addCategory(Intent.CATEGORY_OPENABLE);
        gallery.setType("image/*");

        Intent chooser = Intent.createChooser(gallery, "Send a photo");

        // Camera is best-effort: if the temp file cannot be created we still offer the
        // gallery rather than failing the whole interaction.
        cameraImageUri = null;
        try {
            File photo = File.createTempFile("attach_", ".jpg", getCacheDir());
            cameraImageUri = FileProvider.getUriForFile(
                    this, getPackageName() + ".qtprovider", photo);

            Intent camera = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
            camera.putExtra(MediaStore.EXTRA_OUTPUT, cameraImageUri);
            chooser.putExtra(Intent.EXTRA_INITIAL_INTENTS, new Intent[]{camera});
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "camera capture unavailable; offering the gallery only", e);
            cameraImageUri = null;
        }
        return chooser;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != FILE_CHOOSER_REQUEST) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        // The WebView blocks on this callback. It MUST be answered on every path,
        // including cancellation, or the input is dead for the rest of the session.
        if (filePathCallback == null) return;

        Uri[] results = null;
        if (resultCode == RESULT_OK) {
            if (data != null && data.getData() != null) {
                results = new Uri[]{data.getData()};      // gallery
            } else if (cameraImageUri != null) {
                results = new Uri[]{cameraImageUri};      // camera writes to EXTRA_OUTPUT
            }
        }

        filePathCallback.onReceiveValue(results);
        filePathCallback = null;
        cameraImageUri = null;
    }

    private static WebView findWebView(View view) {
        if (view instanceof WebView) return (WebView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                WebView found = findWebView(group.getChildAt(i));
                if (found != null) return found;
            }
        }
        return null;
    }
}
