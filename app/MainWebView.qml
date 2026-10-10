import QtQuick
import QtWebView

// Android / iOS shell — native platform WebView.
//
// The URL comes from C++ and already carries the secret path prefix
// (AD-08): http://127.0.0.1:<ephemeral>/<64-hex-token>/
//
// This is why there is no JavaScript bridge here and no findWebView() retry
// loop like FunMeet needs: the page authenticates by knowing the URL, and every
// call it makes is ordinary HTTP against the local API. Relative URLs inside the
// bundle inherit the prefix automatically.

Window {
    id: root
    width: 390
    height: 844
    visible: true
    title: qsTr("AnyTime Chat")
    // The page background (--at-bg, light). Without it the window is white for
    // the frames between the launch splash lifting and the page painting.
    color: "#F7F7FA"

    // Tells C++ the launch splash can come down — see the splash block in
    // main.cpp. A failed load counts: the splash must never be the last thing
    // the user sees, whatever went wrong underneath it.
    signal uiReady()

    WebView {
        id: webView
        anchors.fill: parent
        url: appRootUrl

        onLoadingChanged: function (loadRequest) {
            // warn, not log: a UI that cannot load is a real failure, and debug
            // lines are compiled out of release builds.
            if (loadRequest.status === WebView.LoadFailedStatus) {
                console.warn("[AnyTime] UI failed to load:", loadRequest.errorString)
                root.uiReady()
            } else if (loadRequest.status === WebView.LoadSucceededStatus) {
                console.log("[AnyTime] UI loaded")
                root.uiReady()
            }
        }
    }

    // The load signals above are the normal path. This is for the day neither
    // arrives: a splash that never lifts looks exactly like a frozen app.
    Timer {
        interval: 8000
        running: true
        onTriggered: root.uiReady()
    }
}
