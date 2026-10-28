package com.kytyps5.android

import android.app.Application

class KytyApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        CrashDiagnostics.initialize(this)
    }
}
