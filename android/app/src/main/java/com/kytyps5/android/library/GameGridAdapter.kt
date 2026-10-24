package com.kytyps5.android.library

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.kytyps5.android.GameEntry
import com.kytyps5.android.GameLibrary

/** Horizontal landscape cards based on the approved Daedalus-style launcher mockup. */
class GameGridAdapter(
    private val context: Context,
    private val covers: CoverLoader,
    private val coverPx: Int,
    private val onPlay: (GameEntry) -> Unit,
    private val onSettings: (GameEntry) -> Unit,
) : BaseAdapter() {

    var games: List<GameEntry> = emptyList()
        set(value) { field = value; notifyDataSetChanged() }

    private val density = context.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()

    private class Holder(
        val cover: ImageView, val placeholder: TextView, val title: TextView,
        val subtitle: TextView, val badge: TextView, val play: TextView, val gear: TextView,
    )

    override fun getCount() = games.size
    override fun getItem(position: Int) = games[position]
    override fun getItemId(position: Int) = games[position].path.hashCode().toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val view = convertView ?: createCard()
        val h = view.tag as Holder
        val game = getItem(position)
        h.title.text = game.title
        h.subtitle.text = game.titleId ?: if (game.folder.isFile) GameLibrary.imageLabel(game.folder) else "PS5 game"
        h.badge.text = "PS5  •  " + (game.titleId ?: "EXTERNAL")
        h.placeholder.text = game.title.split(' ', '-', '_').filter { it.isNotEmpty() }
            .take(2).joinToString("") { it.take(1).uppercase() }
        h.placeholder.visibility = View.GONE
        covers.load(game.icon, h.cover) { h.placeholder.visibility = View.VISIBLE }
        view.setOnClickListener { onPlay(game) }
        view.setOnLongClickListener { onSettings(game); true }
        h.play.setOnClickListener { onPlay(game) }
        h.gear.setOnClickListener { onSettings(game) }
        return view
    }

    private fun createCard(): View {
        fun rounded(color: Int, radius: Int, stroke: Int? = null) = GradientDrawable().apply {
            cornerRadius = radius.dp.toFloat()
            setColor(color)
            if (stroke != null) setStroke(1.dp, stroke)
        }
        fun text(value: String, sp: Float, color: Int, bold: Boolean = false) =
            TextView(context).apply {
                this.text = value
                textSize = sp
                setTextColor(color)
                if (bold) setTypeface(typeface, Typeface.BOLD)
                maxLines = 2
                ellipsize = TextUtils.TruncateAt.END
            }
        val cover = ImageView(context).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
            background = rounded(Color.rgb(20, 57, 105), 12)
        }
        val placeholder = text("", 30f, Color.rgb(154, 213, 250), true).apply {
            gravity = Gravity.CENTER
            background = rounded(Color.rgb(24, 53, 107), 12)
        }
        val badge = text("PS5", 10f, Color.rgb(147, 210, 249), true)
        val image = FrameLayout(context).apply {
            addView(cover, FrameLayout.LayoutParams(-1, -1))
            addView(placeholder, FrameLayout.LayoutParams(-1, -1))
            addView(badge, FrameLayout.LayoutParams(-1, -2, Gravity.TOP or Gravity.START).apply {
                setMargins(8.dp, 7.dp, 8.dp, 0)
            })
        }
        val name = text("", 17f, Color.WHITE, true)
        val sub = text("", 11f, Color.rgb(159, 178, 203))
        val play = text("▶  Jogar agora", 12f, Color.WHITE, true).apply {
            background = rounded(Color.rgb(39, 124, 206), 11)
            gravity = Gravity.CENTER
            minHeight = 40.dp
        }
        val gear = text("⚙", 19f, Color.rgb(174, 210, 238)).apply {
            background = rounded(Color.rgb(25, 45, 69), 11, Color.rgb(51, 78, 108))
            gravity = Gravity.CENTER
            contentDescription = "Game settings"
        }
        val info = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(10.dp, 12.dp, 9.dp, 10.dp)
            addView(name)
            addView(sub)
            addView(android.widget.Space(context), LinearLayout.LayoutParams(1.dp, 0, 1f))
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(play, LinearLayout.LayoutParams(0, 40.dp, 1f))
                addView(gear, LinearLayout.LayoutParams(40.dp, 40.dp).apply { marginStart = 6.dp })
            })
        }
        return LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            minimumHeight = 150.dp
            background = rounded(Color.rgb(17, 31, 53), 15, Color.rgb(39, 64, 94))
            addView(image, LinearLayout.LayoutParams(0, 150.dp, 0.49f))
            addView(info, LinearLayout.LayoutParams(0, 150.dp, 0.51f))
            tag = Holder(cover, placeholder, name, sub, badge, play, gear)
        }
    }
}
