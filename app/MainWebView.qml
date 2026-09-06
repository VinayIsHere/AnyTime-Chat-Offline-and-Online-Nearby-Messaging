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

    WebView {
        id: webView
        anchors.fill: parent
        url: appRootUrl

        onLoadingChanged: function (loadRequest) {
            if (loadRequest.status === WebView.LoadFailedStatus)
                console.log("[AnyTime] UI failed to load:", loadRequest.errorString)
            else if (loadRequest.status === WebView.LoadSucceededStatus)
                console.log("[AnyTime] UI loaded")
        }
    }
}
