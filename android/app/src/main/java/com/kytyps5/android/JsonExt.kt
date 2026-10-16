package com.kytyps5.android

import org.json.JSONArray
import org.json.JSONObject

/**
 * String value of [key], or [fallback] when it is missing or JSON null. Android's bundled org.json
 * returns the text "null" from optString for JSON null (newer upstream org.json returns the
 * fallback), so every nullable string read goes through here.
 */
fun JSONObject.stringOr(key: String, fallback: String = ""): String =
    if (!has(key) || isNull(key)) fallback else optString(key, fallback)

/** Element [index] as a string, or "" for JSON null (same Android quirk as [stringOr]). */
fun JSONArray.stringAt(index: Int): String = if (isNull(index)) "" else optString(index, "")
