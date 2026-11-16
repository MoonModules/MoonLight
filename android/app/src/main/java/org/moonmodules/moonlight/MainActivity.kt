package org.moonmodules.moonlight

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Base64
import android.view.WindowInsets
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient

/**
 * MoonLight's page, served by the program the service runs, in a WebView: the whole front end, as a browser shows it on a desktop.
 */
class MainActivity : Activity() {

    private lateinit var web: WebView
    private var pickedFiles: ValueCallback<Array<Uri>>? = null
    private var pendingSave: ByteArray? = null   // the file the system's save dialog is choosing a place for

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        askPermissions()
        startForegroundService(Intent(this, MoonLightService::class.java))

        web = WebView(this)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true   // the page keeps its view state in localStorage
        web.webViewClient = object : WebViewClient() {
            // The program takes a moment to start serving, so the first load is retried until it answers.
            override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
                if (request.isForMainFrame) view.postDelayed({ view.reload() }, 500)
            }
        }
        web.webChromeClient = object : WebChromeClient() {
            // A file input (restore a backup, upload a file or an image) opens the system's picker.
            override fun onShowFileChooser(view: WebView, callback: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
                pickedFiles?.onReceiveValue(null)
                pickedFiles = callback
                startActivityForResult(params.createIntent(), PICK_FILES)
                return true
            }
        }
        // A WebView saves what `<a download>` offers to no file, so the page saves a backup, a GIF or a file through this (saveBlob in app.js).
        web.addJavascriptInterface(SaveBridge(), "MoonLightApp")
        // Android 15 draws an app edge to edge, so the page keeps clear of the status and navigation bars a phone has.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            web.setOnApplyWindowInsetsListener { view, insets ->
                val bars = insets.getInsets(WindowInsets.Type.systemBars())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                insets
            }
        }
        setContentView(web)
        web.loadUrl("http://localhost:${MoonLightService.PORT}/")
    }

    @Deprecated("The framework's own result path, which an app without AndroidX has")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == PICK_FILES) {
            pickedFiles?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data))
            pickedFiles = null
        }
        if (requestCode == SAVE_FILE) {
            val bytes = pendingSave
            pendingSave = null
            val place = data?.data
            if (resultCode == RESULT_OK && bytes != null && place != null) contentResolver.openOutputStream(place)?.use { it.write(bytes) }
        }
        super.onActivityResult(requestCode, resultCode, data)
    }

    // A granted microphone joins the service's types, which only a new start applies.
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == ASK_PERMISSIONS) startForegroundService(Intent(this, MoonLightService::class.java))
    }

    @Deprecated("The framework's own back path, which an app without AndroidX has")
    override fun onBackPressed() {
        if (web.canGoBack()) web.goBack() else super.onBackPressed()
    }

    // The microphone for the Audio card, and on Android 13 and later the notification that carries Stop.
    private fun askPermissions() {
        val wanted = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) wanted += Manifest.permission.POST_NOTIFICATIONS
        val missing = wanted.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), ASK_PERMISSIONS)
    }

    /** The page's save: the bytes, then the system's dialog for where they go, as a browser asks where a download goes. */
    inner class SaveBridge {
        @JavascriptInterface fun save(name: String, mime: String, base64: String) {
            val bytes = Base64.decode(base64, Base64.DEFAULT)
            runOnUiThread {
                if (pendingSave != null) return@runOnUiThread   // one dialog at a time, so its place receives its own file
                pendingSave = bytes
                val ask = Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(mime).putExtra(Intent.EXTRA_TITLE, name)
                startActivityForResult(ask, SAVE_FILE)
            }
        }
    }

    companion object {
        private const val PICK_FILES = 1
        private const val ASK_PERMISSIONS = 2
        private const val SAVE_FILE = 3
    }
}
